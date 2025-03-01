#include "hook.h"
#include <dlfcn.h>

#include "config.h"
#include "log.h"
#include "fiber.h"
#include "iomanager.h"
#include "fd_manager.h"
#include "macro.h"

sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");
namespace sylar {

static sylar::ConfigVar<int>::ptr g_tcp_connect_timeout =
    sylar::Config::Lookup("tcp.connect.timeout", 5000, "tcp connect timeout");

static thread_local bool t_hook_enable = false;

#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt)

void hook_init() {
    static bool is_inited = false;
    if(is_inited) {
        return;
    }
#define XX(name) name ## _f = (name ## _fun)dlsym(RTLD_NEXT, #name);
    HOOK_FUN(XX);
#undef XX
}

static uint64_t s_connect_timeout = -1;
struct _HookIniter {
    _HookIniter() {
        hook_init();
        s_connect_timeout = g_tcp_connect_timeout->getValue();

        g_tcp_connect_timeout->addListener([](const int& old_value, const int& new_value){
                SYLAR_LOG_INFO(g_logger) << "tcp connect timeout changed from "
                                         << old_value << " to " << new_value;
                s_connect_timeout = new_value;
        });
    }
};

static _HookIniter s_hook_initer;

bool is_hook_enable() {
    return t_hook_enable;
}

void set_hook_enable(bool flag) {
    t_hook_enable = flag;
}

}

struct timer_info {
    int cancelled = 0;
};

/*
    函数的主要作用是对 I/O 操作进行 Hook，支持协程调度和超时处理。具体步骤如下：

    1.检查是否启用 Hook，如果未启用则直接调用原始系统调用。
    2.获取文件描述符上下文，检查文件描述符是否有效、是否关闭、是否为非阻塞模式。
    3.获取超时时间，并创建定时器信息对象。
    4.执行原始系统调用，如果遇到 EINTR 错误则重试。
    5.如果遇到 EAGAIN 错误，则向 IOManager 注册事件，并让出当前协程，等待事件触发或超时。
    6.当事件触发或超时时，重新尝试执行原始系统调用。
    7.返回系统调用的结果

    fd：文件描述符。
    fun：原始的系统调用函数指针。
    hook_fun_name：钩子函数的名称，用于日志记录。
    event：事件类型（如读事件或写事件）。
    timeout_so：超时选项（如 SO_RCVTIMEO 或 SO_SNDTIMEO）。
    args：传递给原始系统调用的其他参数。
*/
template<typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char* hook_fun_name,
        uint32_t event, int timeout_so, Args&&... args) {
    // 检查是否启用 Hook
    if(!sylar::t_hook_enable) {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 获取文件描述符上下文
    sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(fd);
    if(!ctx) {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 检查文件描述符是否有效、是否关闭、是否为非阻塞模式
    if(ctx->isClose()) {
        errno = EBADF;
        return -1;
    }

    // 如果文件描述符不是套接字或已设置为用户非阻塞模式,则直接调用原始系统调用
    if(!ctx->isSocket() || ctx->getUserNonblock()) {
        return fun(fd, std::forward<Args>(args)...);
    }

    // 获取超时时间，并创建定时器信息对象
    uint64_t to = ctx->getTimeout(timeout_so);
    std::shared_ptr<timer_info> tinfo(new timer_info);

retry:
    ssize_t n = fun(fd, std::forward<Args>(args)...);

    // 如果遇到 EINTR 错误则重试
    while(n == -1 && errno == EINTR) {
        n = fun(fd, std::forward<Args>(args)...);
    }
    // 如果遇到 EAGAIN 错误，则向 IOManager 注册事件
    // 并让出当前协程，等待事件触发或超时
    if(n == -1 && errno == EAGAIN) {
        sylar::IOManager* iom = sylar::IOManager::GetThis();
        sylar::Timer::ptr timer;
        std::weak_ptr<timer_info> winfo(tinfo);

        //如果设置了超时时间，则添加一个定时器，当超时时取消事件
        if(to != (uint64_t)-1) {
            timer = iom->addConditionTimer(to, [winfo, fd, iom, event]() {
                auto t = winfo.lock();
                if(!t || t->cancelled) {
                    return;
                }
                t->cancelled = ETIMEDOUT;
                //这个取消事件，是会先触发回调再结束的
                iom->cancelEvent(fd, (sylar::IOManager::Event)(event));
            }, winfo);
        }
        
        //注册事件
        int rt = iom->addEvent(fd, (sylar::IOManager::Event)(event));
        //注册失败
        if(SYLAR_UNLIKELY(rt)) {
            SYLAR_LOG_ERROR(g_logger) << hook_fun_name << " addEvent("
                << fd << ", " << event << ")";
            if(timer) {
                timer->cancel();
            }
            return -1;
        }
        //注册事件成功，则让出当前协程，等待事件触发
        else {
            sylar::Fiber::GetThis()->yield();
            if(timer) {
                timer->cancel();
            }
            //如果定时器标记了 cancelled，说明还没等到 I/O 事件就超时或被取消，但事件是以及被处理了
            /*
                这里的一个疑问：内部返回错误代码给外面调用的，但事件已经被执行了，这样是否合理
                回答：是合理的。即使事件被“强制触发”了（如定时器超时后调用 cancelEvent），
                也不代表 I/O 成功，需向上层报告出错（如超时）。
                这样上层逻辑才能知道此次操作已失败或被取消，而不是成功完成。
            */
            if(tinfo->cancelled) {
                errno = tinfo->cancelled;
                return -1;
            }
            // 重新执行原始系统调用
            goto retry;
        }
    }
    
    return n;
}


extern "C" {
#define XX(name) name ## _fun name ## _f = nullptr;
    HOOK_FUN(XX);
#undef XX

/*
    1.检查是否启用 Hook，如果未启用则直接调用原始系统调用。
    2.创建一个新的 Fiber 对象，将当前协程保存到 Fiber::t_fiber 中。
    3.调用 IOManager::GetThis() 获取当前 IOManager 对象。
    4.调用 IOManager::addTimer() 添加一个定时器，当定时器超时时，通过 IOManager::schedule() 调度当前协程。
    5.让当前协程让出 CPU，等待定时器超时。
*/
unsigned int sleep(unsigned int seconds) {
    if(!sylar::t_hook_enable) {
        return sleep_f(seconds);
    }

    sylar::Fiber::ptr fiber = sylar::Fiber::GetThis();
    sylar::IOManager* iom = sylar::IOManager::GetThis();
    iom->addTimer(seconds * 1000, std::bind((void(sylar::Scheduler::*)
            (sylar::Fiber::ptr, int thread))&sylar::IOManager::schedule
            ,iom, fiber, -1));
    sylar::Fiber::GetThis()->yield();
    return 0;
}

/*
    1.检查是否启用 Hook，如果未启用则直接调用原始系统调用。
    2.创建一个新的 Fiber 对象，将当前协程保存到 Fiber::t_fiber 中。
    3.调用 IOManager::GetThis() 获取当前 IOManager 对象。
    4.调用 IOManager::addTimer() 添加一个定时器，当定时器超时时，通过 IOManager::schedule() 调度当前协程。
    5.让当前协程让出 CPU，等待定时器超时。
*/
int usleep(useconds_t usec) {
    if(!sylar::t_hook_enable) {
        return usleep_f(usec);
    }
    sylar::Fiber::ptr fiber = sylar::Fiber::GetThis();
    sylar::IOManager* iom = sylar::IOManager::GetThis();
    iom->addTimer(usec / 1000, std::bind((void(sylar::Scheduler::*)
            (sylar::Fiber::ptr, int thread))&sylar::IOManager::schedule
            ,iom, fiber, -1));
    sylar::Fiber::GetThis()->yield();
    return 0;
}

/*
    1.检查是否启用 Hook，如果未启用则直接调用原始系统调用。
    2.计算超时时间，将秒和纳秒转换为毫秒。
    3.创建一个新的 Fiber 对象，将当前协程保存到 Fiber::t_fiber 中。
    4.调用 IOManager::GetThis() 获取当前 IOManager 对象。
    5.调用 IOManager::addTimer() 添加一个定时器，当定时器超时时，通过 IOManager::schedule() 调度当前协程。
    6.让当前协程让出 CPU，等待定时器超时。
*/
int nanosleep(const struct timespec *req, struct timespec *rem) {
    if(!sylar::t_hook_enable) {
        return nanosleep_f(req, rem);
    }

    int timeout_ms = req->tv_sec * 1000 + req->tv_nsec / 1000 /1000;
    sylar::Fiber::ptr fiber = sylar::Fiber::GetThis();
    sylar::IOManager* iom = sylar::IOManager::GetThis();
    iom->addTimer(timeout_ms, std::bind((void(sylar::Scheduler::*)
            (sylar::Fiber::ptr, int thread))&sylar::IOManager::schedule
            ,iom, fiber, -1));
    sylar::Fiber::GetThis()->yield();
    return 0;
}

/*
    创建一个新的 socket 文件描述符。
    如果启用了 Hook，则在创建 socket 后，将其添加到 FdMgr 中进行管理。
*/
int socket(int domain, int type, int protocol) {
    if(!sylar::t_hook_enable) {
        return socket_f(domain, type, protocol);
    }
    int fd = socket_f(domain, type, protocol);
    if(fd == -1) {
        return fd;
    }
    sylar::FdMgr::GetInstance()->get(fd, true);
    return fd;
}

/*
    1.判断传入的fd是否为套接字，如果不为套接字，则调用系统的connect函数并返回。
    2.判断fd是否被显式设置为了非阻塞模式，如果是则调用系统的connect函数并返回。
    3.调用系统的connect函数，由于套接字是非阻塞的，这里会直接返回EINPROGRESS错误。
    4.如果超时参数有效，则添加一个条件定时器，在定时时间到后通过t->cancelled设置超时标志并触发一次WRITE事件。
    5.添加WRITE事件并yield，等待WRITE事件触发再往下执行。
    6.等待超时或套接字可写，如果先超时，则条件变量winfo仍然有效，通过winfo来设置超时标志并触发WRITE事件，协程从yield点返回，返回之后通过超时标志设置errno并返回-1；
    如果在未超时之前套接字就可写了，那么直接取消定时器并返回成功。取消定时器会导致定时器回调被强制执行一次，但这并不会导致问题，因为只有当前协程结束后，定时器回调才会在接下来被调度，
    由于定时器回调被执行时connect_with_timeout协程已经执行完了，所以理所当然地条件变量也被释放了，所以实际上定时器回调函数什么也没做。
*/
int connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout_ms) {
    if(!sylar::t_hook_enable) {
        return connect_f(fd, addr, addrlen);
    }
    sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(fd);
    if(!ctx || ctx->isClose()) {
        errno = EBADF;
        return -1;
    }

    if(!ctx->isSocket()) {
        return connect_f(fd, addr, addrlen);
    }

    if(ctx->getUserNonblock()) {
        return connect_f(fd, addr, addrlen);
    }

    int n = connect_f(fd, addr, addrlen);
    if(n == 0) {
        return 0;
    } else if(n != -1 || errno != EINPROGRESS) {
        return n;
    }

    sylar::IOManager* iom = sylar::IOManager::GetThis();
    sylar::Timer::ptr timer;
    std::shared_ptr<timer_info> tinfo(new timer_info);
    std::weak_ptr<timer_info> winfo(tinfo);

    if(timeout_ms != (uint64_t)-1) {
        timer = iom->addConditionTimer(timeout_ms, [winfo, fd, iom]() {
                auto t = winfo.lock();
                if(!t || t->cancelled) {
                    return;
                }
                t->cancelled = ETIMEDOUT;
                iom->cancelEvent(fd, sylar::IOManager::WRITE);
        }, winfo);
    }

    int rt = iom->addEvent(fd, sylar::IOManager::WRITE);
    if(rt == 0) {
        sylar::Fiber::GetThis()->yield();
        if(timer) {
            timer->cancel();
        }
        if(tinfo->cancelled) {
            errno = tinfo->cancelled;
            return -1;
        }
    } else {
        if(timer) {
            timer->cancel();
        }
        SYLAR_LOG_ERROR(g_logger) << "connect addEvent(" << fd << ", WRITE) error";
    }

    int error = 0;
    socklen_t len = sizeof(int);
    if(-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len)) {
        return -1;
    }
    if(!error) {
        return 0;
    } else {
        errno = error;
        return -1;
    }
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return connect_with_timeout(sockfd, addr, addrlen, sylar::s_connect_timeout);
}

/*
    accept 函数用于处理接受连接操作。
    使用 do_io 函数进行 Hook，支持协程调度和超时处理。
    如果接受连接成功，则将新创建的文件描述符添加到 FdMgr 中进行管理。
*/
int accept(int s, struct sockaddr *addr, socklen_t *addrlen) {
    int fd = do_io(s, accept_f, "accept", sylar::IOManager::READ, SO_RCVTIMEO, addr, addrlen);
    if(fd >= 0) {
        sylar::FdMgr::GetInstance()->get(fd, true);
    }
    return fd;
}

ssize_t read(int fd, void *buf, size_t count) {
    return do_io(fd, read_f, "read", sylar::IOManager::READ, SO_RCVTIMEO, buf, count);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    return do_io(fd, readv_f, "readv", sylar::IOManager::READ, SO_RCVTIMEO, iov, iovcnt);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return do_io(sockfd, recv_f, "recv", sylar::IOManager::READ, SO_RCVTIMEO, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
    return do_io(sockfd, recvfrom_f, "recvfrom", sylar::IOManager::READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    return do_io(sockfd, recvmsg_f, "recvmsg", sylar::IOManager::READ, SO_RCVTIMEO, msg, flags);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return do_io(fd, write_f, "write", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, count);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    return do_io(fd, writev_f, "writev", sylar::IOManager::WRITE, SO_SNDTIMEO, iov, iovcnt);
}

ssize_t send(int s, const void *msg, size_t len, int flags) {
    return do_io(s, send_f, "send", sylar::IOManager::WRITE, SO_SNDTIMEO, msg, len, flags);
}

ssize_t sendto(int s, const void *msg, size_t len, int flags, const struct sockaddr *to, socklen_t tolen) {
    return do_io(s, sendto_f, "sendto", sylar::IOManager::WRITE, SO_SNDTIMEO, msg, len, flags, to, tolen);
}

ssize_t sendmsg(int s, const struct msghdr *msg, int flags) {
    return do_io(s, sendmsg_f, "sendmsg", sylar::IOManager::WRITE, SO_SNDTIMEO, msg, flags);
}

/*
    close 函数用于关闭文件描述符。
    如果启用了 Hook，则在关闭文件描述符前，
    取消所有与该文件描述符相关的事件，并将其从 FdMgr 中删除。
*/
int close(int fd) {
    if(!sylar::t_hook_enable) {
        return close_f(fd);
    }

    sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(fd);
    if(ctx) {
        auto iom = sylar::IOManager::GetThis();
        if(iom) {
            iom->cancelAll(fd);
        }
        sylar::FdMgr::GetInstance()->del(fd);
    }
    return close_f(fd);
}

/*
    fcntl 函数用于控制文件描述符。
    如果启用了 Hook，则在设置非阻塞模式时，
    同时设置用户非阻塞标志，并将其保存到 FdCtx 中。
*/
int fcntl(int fd, int cmd, ... /* arg */ ) {
    va_list va;
    va_start(va, cmd);
    switch(cmd) {
        case F_SETFL:
            {
                int arg = va_arg(va, int);
                va_end(va);
                sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(fd);
                if(!ctx || ctx->isClose() || !ctx->isSocket()) {
                    return fcntl_f(fd, cmd, arg);
                }
                ctx->setUserNonblock(arg & O_NONBLOCK);
                if(ctx->getSysNonblock()) {
                    arg |= O_NONBLOCK;
                } else {
                    arg &= ~O_NONBLOCK;
                }
                return fcntl_f(fd, cmd, arg);
            }
            break;
        case F_GETFL:
            {
                va_end(va);
                int arg = fcntl_f(fd, cmd);
                sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(fd);
                if(!ctx || ctx->isClose() || !ctx->isSocket()) {
                    return arg;
                }
                if(ctx->getUserNonblock()) {
                    return arg | O_NONBLOCK;
                } else {
                    return arg & ~O_NONBLOCK;
                }
            }
            break;
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
            {
                int arg = va_arg(va, int);
                va_end(va);
                return fcntl_f(fd, cmd, arg); 
            }
            break;
        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
            {
                va_end(va);
                return fcntl_f(fd, cmd);
            }
            break;
        case F_SETLK:
        case F_SETLKW:
        case F_GETLK:
            {
                struct flock* arg = va_arg(va, struct flock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;
        case F_GETOWN_EX:
        case F_SETOWN_EX:
            {
                struct f_owner_exlock* arg = va_arg(va, struct f_owner_exlock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;
        default:
            va_end(va);
            return fcntl_f(fd, cmd);
    }
}

/*
    ioctl 函数用于控制设备。
    如果启用了 Hook，则在设置非阻塞模式时，
    同时设置用户非阻塞标志，并将其保存到 FdCtx 中。
*/
int ioctl(int d, unsigned long int request, ...) {
    va_list va;
    va_start(va, request);
    void* arg = va_arg(va, void*);
    va_end(va);

    if(FIONBIO == request) {
        bool user_nonblock = !!*(int*)arg;
        sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(d);
        if(!ctx || ctx->isClose() || !ctx->isSocket()) {
            return ioctl_f(d, request, arg);
        }
        ctx->setUserNonblock(user_nonblock);
    }
    return ioctl_f(d, request, arg);
}

/*
    getsockopt 函数用于获取套接字选项。
    如果启用了 Hook，则在获取超时时间时，
    将其保存到 FdCtx 中。
*/
int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    return getsockopt_f(sockfd, level, optname, optval, optlen);
}

/*
    setsockopt 函数用于设置套接字选项。
    如果启用了 Hook，则在设置超时时间时，
    将其保存到 FdCtx 中。
*/
int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    if(!sylar::t_hook_enable) {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }
    if(level == SOL_SOCKET) {
        if(optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
            sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(sockfd);
            if(ctx) {
                const timeval* v = (const timeval*)optval;
                ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
            }
        }
    }
    return setsockopt_f(sockfd, level, optname, optval, optlen);
}

}

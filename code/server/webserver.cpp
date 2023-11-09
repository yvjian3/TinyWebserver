#include "webserver.h"

using namespace std;

// 构造函数
WebServer::WebServer(
            int port, int trigMode, int timeoutMS, bool OptLinger,
            int sqlPort, const char* sqlUser, const  char* sqlPwd,
            const char* dbName, int connPoolNum, int threadNum,
            bool openLog, int logLevel, int logQueSize):
            port_(port), openLinger_(OptLinger), timeoutMS_(timeoutMS), isClose_(false),
            timer_(new HeapTimer()), threadpool_(new ThreadPool(threadNum)), epoller_(new Epoller())
    {
    // 初始化资源的目录
    srcDir_ = getcwd(nullptr, 256); // 获取当前的工作目录
    // /home/wjy3919/WebServer/resources/
    assert(srcDir_);
    strncat(srcDir_, "/resources/", 16);

    // 初始化静态变量
    HttpConn::userCount = 0;    // 用户数，有多少个客户端连接进来
    HttpConn::srcDir = srcDir_; // 资源目录，赋值给HttpConn类，供其使用
    // 连接池
    SqlConnPool::Instance()->Init("localhost", sqlPort, sqlUser, sqlPwd, dbName, connPoolNum);

    // 初始化事件的模式（ET模式还是LT模式）
    InitEventMode_(trigMode);
    // 初始化Socket
    if(!InitSocket_()) { isClose_ = true;}  // 初始化成功就继续往下执行，初始化失败就关闭服务器

    // 对日志进行初始化的操作
    if(openLog) {
        Log::Instance()->init(logLevel, "./log", ".log", logQueSize);
        if(isClose_) { LOG_ERROR("========== Server init error!=========="); }
        else {
            LOG_INFO("========== Server init ==========");
            LOG_INFO("Port:%d, OpenLinger: %s", port_, OptLinger? "true":"false");
            LOG_INFO("Listen Mode: %s, OpenConn Mode: %s",
                            (listenEvent_ & EPOLLET ? "ET": "LT"),
                            (connEvent_ & EPOLLET ? "ET": "LT"));
            LOG_INFO("LogSys level: %d", logLevel);
            LOG_INFO("srcDir: %s", HttpConn::srcDir);
            LOG_INFO("SqlConnPool num: %d, ThreadPool num: %d", connPoolNum, threadNum);
        }
    }
}

// 析构函数
WebServer::~WebServer() {
    close(listenFd_);
    isClose_ = true;
    free(srcDir_);
    SqlConnPool::Instance()->ClosePool();
}

// 设置监听的文件描述符和通信的文件描述符的模式
void WebServer::InitEventMode_(int trigMode) {
    listenEvent_ = EPOLLRDHUP;  // 检测对方能否正常关闭
    // 设置EPOLLONESHOT，使一个socket连接在任一时刻都只被一个线程处理
    connEvent_ = EPOLLONESHOT | EPOLLRDHUP;
    switch (trigMode)
    {
    case 0:
        break;
    case 1:
        connEvent_ |= EPOLLET;
        break;
    case 2:
        listenEvent_ |= EPOLLET;
        break;
    case 3:
        listenEvent_ |= EPOLLET;
        connEvent_ |= EPOLLET;
        break;
    default:
        listenEvent_ |= EPOLLET;
        connEvent_ |= EPOLLET;
        break;
    }
    HttpConn::isET = (connEvent_ & EPOLLET);
}

void WebServer::Start() {
    int timeMS = -1;  /* epoll wait timeout == -1 无事件将阻塞 */
    if(!isClose_) { LOG_INFO("========== Server start =========="); }
    /**
     * 在主线程
     * 只要服务器没有关闭，就一直运行
     * 因为要一直调用epoll帮忙检测有没有数据到达
    */
    while(!isClose_) {
        // 解决超时连接
        if(timeoutMS_ > 0) {
            timeMS = timer_->GetNextTick();
        }
        // 不断调用epoll_wait去监测有没有事件到达，返回值为监测到有多少个
        // 检测timeMS时间，如果检测到事件就返回，如果一直没检测到事件超过这个时间也返回
        // 不一直阻塞是因为如果一直没有事件到达就不能返回回来关闭超时连接了
        int eventCnt = epoller_->Wait(timeMS);    

        for(int i = 0; i < eventCnt; i++) {
            /* 处理事件 */
            int fd = epoller_->GetEventFd(i);   // 先获取要检测的文件描述符的fd
            uint32_t events = epoller_->GetEvents(i);   // 获取检测的事件
            // 如果检测到的文件描述符和监听文件描述符一样，就去处理监听事件，也就是accept接收新连接
            // 监听文件描述符有数据代表有新的客户端连接
            if(fd == listenFd_) {
                DealListen_();      //处理监听事件，接收客户端连接
            }
            // 连接出现错误，就把和这个文件描述符的连接给关闭掉
            else if(events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                assert(users_.count(fd) > 0);
                CloseConn_(&users_[fd]);    // 关闭连接
            }
            // 如果读事件产生了，就处理读操作
            // 监听到读事件，说明连接请求发送过来了，发送到了服务器的TCP接收缓冲区
            else if(events & EPOLLIN) {
                assert(users_.count(fd) > 0);
                DealRead_(&users_[fd]);     //处理读操作
            }
            // 如果是写事件，就去处理写操作
            // 检测到可以写，就处理DealWrite
            else if(events & EPOLLOUT) {
                assert(users_.count(fd) > 0);
                DealWrite_(&users_[fd]);    // 处理写操作
            } else {
                LOG_ERROR("Unexpected event");
            }
        }
    }
}

void WebServer::SendError_(int fd, const char*info) {
    assert(fd > 0);
    int ret = send(fd, info, strlen(info), 0);  //给这个文件描述符发送数据
    if(ret < 0) {
        LOG_WARN("send error to client[%d] error!", fd);
    }
    close(fd);  // 把这个客户端关闭掉
}

void WebServer::CloseConn_(HttpConn* client) {
    assert(client);
    LOG_INFO("Client[%d] quit!", client->GetFd());
    // 从epoller中将这个文件描述符删掉
    epoller_->DelFd(client->GetFd());
    // 客户端关闭
    client->Close();
}

void WebServer::AddClient_(int fd, sockaddr_in addr) {
    assert(fd > 0);
    // users_是map集合，保存用户信息的，键是文件描述符，值是HttpConnection（连接相关的信息都保存在里面）
    users_[fd].init(fd, addr);
    if(timeoutMS_ > 0) {
        timer_->add(fd, timeoutMS_, std::bind(&WebServer::CloseConn_, this, &users_[fd]));
    }
    // 把新连接进来的fd添加到epoller身上，监测有没有数据到达
    epoller_->AddFd(fd, EPOLLIN | connEvent_);
    SetFdNonblock(fd);  // 设置非阻塞
    LOG_INFO("Client[%d] in!", users_[fd].GetFd());
}

// 处理监听
void WebServer::DealListen_() {
    struct sockaddr_in addr;    // 保存连接的客户端的信息
    socklen_t len = sizeof(addr);
    do {
        // 将客户端的信息保存到addr当中，一次一个，当都连接完了，没有客户端需要连接了返回-1
        // 接受新的连接
        int fd = accept(listenFd_, (struct sockaddr *)&addr, &len);
        // 小于等于0表示出错了就返回
        if(fd <= 0) { return;}  
        // fd>0就是连接成功了，但是有最大客户端数量，需要判断一下
        // 如果当前用户连接的客户端的数量>=最大的文件描述符的数量，就通知服务器繁忙，
        else if(HttpConn::userCount >= MAX_FD) {
            SendError_(fd, "Server busy!");
            LOG_WARN("Clients is full!");
            return;
        }
        AddClient_(fd, addr);   // 添加客户端
    } while(listenEvent_ & EPOLLET);
}

void WebServer::DealRead_(HttpConn* client) {
    assert(client);
    // 处理读事件了即有数据传输了，就延长这个客户端的超时时间
    ExtentTime_(client);
    // Reactor模式，主线程不读数据，读写操作和处理逻辑都交给子线程
    // 把客户端的信息添加到线程池里面，让子线程去处理
    threadpool_->AddTask(std::bind(&WebServer::OnRead_, this, client));
}

void WebServer::DealWrite_(HttpConn* client) {
    assert(client);
    ExtentTime_(client);
    // 添加到线程池里，让子线程去执行写事件
    threadpool_->AddTask(std::bind(&WebServer::OnWrite_, this, client));
}

void WebServer::ExtentTime_(HttpConn* client) {
    assert(client);
    if(timeoutMS_ > 0) { timer_->adjust(client->GetFd(), timeoutMS_); }
}

// 这个方法是在子线程中执行的
void WebServer::OnRead_(HttpConn* client) {
    assert(client);
    int ret = -1;
    int readErrno = 0;
    ret = client->read(&readErrno);     // 读取客户端的数据，读到client里
    if(ret <= 0 && readErrno != EAGAIN) {
        CloseConn_(client);
        return;
    }
    // 处理，业务逻辑的处理
    OnProcess(client);
}

// 子线程执行
void WebServer::OnProcess(HttpConn* client) {
    // 客户端去处理逻辑
    if(client->process()) {
        // 如果处理业务逻辑成功了，就修改该客户端epoller监听的文件描述符，监听是否可写的事件
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
    } else {
        epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLIN);
    }
}

// 子线程执行
void WebServer::OnWrite_(HttpConn* client) {
    assert(client);
    int ret = -1;
    int writeErrno = 0;
    ret = client->write(&writeErrno);
    if(client->ToWriteBytes() == 0) {
        /* 传输完成 */
        if(client->IsKeepAlive()) {
            OnProcess(client);
            return;
        }
    }
    else if(ret < 0) {
        if(writeErrno == EAGAIN) {
            /* 继续传输 */
            epoller_->ModFd(client->GetFd(), connEvent_ | EPOLLOUT);
            return;
        }
    }
    CloseConn_(client);
}

/* Create listenFd */
bool WebServer::InitSocket_() {
    int ret;
    struct sockaddr_in addr;    // 套接字地址
    // 判断端口号大小是否符合
    if(port_ > 65535 || port_ < 1024) {
        LOG_ERROR("Port:%d error!",  port_);
        return false;
    }
    // 地址组类型，IPv4
    addr.sin_family = AF_INET;
    //host主机字节序转换为网络字节序
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 转换的是ip地址，传递INADDR_ANY为绑定任何可以绑定的ip地址
    addr.sin_port = htons(port_);   // 端口号转换 

    struct linger optLinger = { 0 };
    if(openLinger_) {
        /* 优雅关闭: 直到所剩数据发送完毕或超时 */
        optLinger.l_onoff = 1;
        optLinger.l_linger = 1;
    }
    // 创建一个socket，返回一个监听的文件描述符
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(listenFd_ < 0) {
        LOG_ERROR("Create socket error!", port_);
        return false;
    }

    ret = setsockopt(listenFd_, SOL_SOCKET, SO_LINGER, &optLinger, sizeof(optLinger));
    if(ret < 0) {
        close(listenFd_);
        LOG_ERROR("Init linger error!", port_);
        return false;
    }

    int optval = 1;
    /* 端口复用 */
    /* 只有最后一个套接字会正常接收数据。 */
    ret = setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));
    if(ret == -1) {
        LOG_ERROR("set socket setsockopt error !");
        close(listenFd_);
        return false;
    }

    // 绑定，传递监听的文件描述符，传递地址
    ret = bind(listenFd_, (struct sockaddr *)&addr, sizeof(addr));
    if(ret < 0) {
        LOG_ERROR("Bind Port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    // 监听listen()
    ret = listen(listenFd_, 6);
    if(ret < 0) {
        LOG_ERROR("Listen port:%d error!", port_);
        close(listenFd_);
        return false;
    }

    /**
     * 把监听的文件描述符加到epoll身上
     * 通过epoll检测listenfd有没有数据到达
     * 有数据到达说明有客户端连接进来
    */
    ret = epoller_->AddFd(listenFd_,  listenEvent_ | EPOLLIN);
    if(ret == 0) {
        LOG_ERROR("Add listen error!");
        close(listenFd_);
        return false;
    }
    SetFdNonblock(listenFd_);   // 设置监听文件描述符非阻塞
    LOG_INFO("Server port:%d", port_);
    return true;
}

// 设置文件描述符非阻塞
int WebServer::SetFdNonblock(int fd) {
    assert(fd > 0);
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) | O_NONBLOCK);
}



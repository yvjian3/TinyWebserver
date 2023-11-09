#include "sqlconnpool.h"
using namespace std;

SqlConnPool::SqlConnPool() {
    useCount_ = 0;
    freeCount_ = 0;
}

/**
 * 单例模式，懒汉式，在第一次调用Instance()方法时实例化，在类加载时并不自行实例化
 * 这种技术又称为“延迟加载”技术，即需要的时候再加载实例，
 * C++11中可以保证static变量是多线程安全的，在底层实现了加锁操作，所以不需要像以前那样自己写加锁操作
 * 由于是一个static对象，可以保证对象只生成一次；
 * 程序结束时，系统会调用对应的析构函数；如果是new出来的对象，程序结束时，系统不会自动调用析构函数
*/   
SqlConnPool* SqlConnPool::Instance() {
    static SqlConnPool connPool;
    return &connPool;
}

// 初始化
void SqlConnPool::Init(const char* host, int port,
            const char* user,const char* pwd, const char* dbName,
            int connSize = 10) {    // 默认池子里开始创建10个连接
    assert(connSize > 0);
    for (int i = 0; i < connSize; i++) {
        // 可以认为就是MYSQL的一个连接
        MYSQL *sql = nullptr;
        // 初始化
        sql = mysql_init(sql);
        if (!sql) {
            LOG_ERROR("MySql init error!");
            assert(sql);
        }
        // 连接数据库
        sql = mysql_real_connect(sql, host,
                                 user, pwd,
                                 dbName, port, nullptr, 0);
        if (!sql) {
            LOG_ERROR("MySql Connect error!");
        }
        // 放到连接队列里面
        connQue_.push(sql);
    }
    MAX_CONN_ = connSize;
    // 初始化信号量
    sem_init(&semId_, 0, MAX_CONN_);
}

// 获取一个MySQL的连接
MYSQL* SqlConnPool::GetConn() {
    MYSQL *sql = nullptr;
    if(connQue_.empty()){
        LOG_WARN("SqlConnPool busy!");
        return nullptr;
    }
    // 如果大于0就执行，等于0代表没有没有可以取的连接，就在这等待
    sem_wait(&semId_);
    {
        lock_guard<mutex> locker(mtx_);
        // 取到数据就拿出来第一个，移除掉第一个
        sql = connQue_.front();
        connQue_.pop();
    }
    return sql;
}

// 释放一个数据库连接（不是真的释放，是放到池子里）
void SqlConnPool::FreeConn(MYSQL* sql) {
    assert(sql);
    lock_guard<mutex> locker(mtx_);
    // 放到池子里面
    connQue_.push(sql);
    // semId_加一，连接池里面连接多了一个
    sem_post(&semId_);
}

// 关闭池子
void SqlConnPool::ClosePool() { 
    lock_guard<mutex> locker(mtx_);
    while(!connQue_.empty()) {
        auto item = connQue_.front();
        connQue_.pop();
        mysql_close(item);
    }
    // 关闭MySQL整体的资源
    mysql_library_end();        
}

// 获取空闲的用户的数量
int SqlConnPool::GetFreeConnCount() {
    lock_guard<mutex> locker(mtx_);
    return connQue_.size();
}

SqlConnPool::~SqlConnPool() {
    ClosePool();
}

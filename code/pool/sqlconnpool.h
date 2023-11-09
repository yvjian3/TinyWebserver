#ifndef SQLCONNPOOL_H
#define SQLCONNPOOL_H

#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <semaphore.h>
#include <thread>
#include "../log/log.h"

/**
 * 也是一个生产者消费者模型，和线程池一样
 * 原理也是，在一开始就创建出一些连接对象出来，要用的时候就直接从池子里面那一个连接对象去用就行了
 * 不用了以后就放到池子里面，不断开它，下一次可以继续去使用
*/
class SqlConnPool {
public:
    static SqlConnPool *Instance();     // 提供一个方法访问静态实例（单例模式）

    MYSQL *GetConn();   // 获取一个MySQL的连接
    void FreeConn(MYSQL * conn);    // 释放一个数据库连接（不是真的释放，是放到池子里）
    int GetFreeConnCount();     // 获取空闲的用户的数量

    // 初始化
    void Init(const char* host, int port,
              const char* user,const char* pwd, 
              const char* dbName, int connSize);
              
    void ClosePool();   // 关闭池子

private:
    // 单例模式，所以构造函数私有化
    SqlConnPool();  // 构造函数
    ~SqlConnPool(); // 析构函数

    int MAX_CONN_;  // 最大连接数
    int useCount_;  // 当前的用户数
    int freeCount_; // 空闲的用户数（还可以连接几个用户）

    std::queue<MYSQL *> connQue_;   // 队列（MySQL *），用来操作MySQL数据库的
    std::mutex mtx_;    // 互斥锁
    sem_t semId_;   // 信号量
};


#endif // SQLCONNPOOL_H

#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template <typename T>
class threadpool
{
public:

    /* 构造函数
       actor_model:工作模式
       connPool:数据库连接池指针
       thread_number:线程池中线程的数量
       max_requests:请求队列中最多允许的、等待处理的请求的数量 */
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_requests = 10000);
    
     // 析构函数
    ~threadpool();
    
    // 向请求队列中添加任务
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();     // 工作队列任务处理函数

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue;  //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //请求队列中是否有任务需要处理
    connection_pool *m_connPool;  //数据库
    int m_actor_model;          //模型切换
};
template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) :
        m_actor_model(actor_model), m_thread_number(thread_number), m_max_requests(max_requests),
        m_threads(NULL), m_connPool(connPool)
{
    // 线程数和允许的最大请求数均小等于0，出错
    if (thread_number <= 0 || max_requests <= 0)
    { 
        throw std::exception();
    }
    // 初始化线程，分配id
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    // 创建thread_number 个线程，并将他们设置为脱离线程。
    for (int i = 0; i < thread_number; i++)
    {
        // 循环创建线程，1标识符，2线程属性（NULL为默认），3指定线程将运行的函数，4运行的参数
        // 函数原型中的第三个参数，为函数指针，指向处理线程函数的地址。
        // 该函数，要求为静态函数。如果处理线程函数为类成员函数时，需要将其设置为静态成员函数。
        // 但静态成员函数不能访问非静态成员变量，所以通过this传递到arg中，再通过它去访问成员变量
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete []m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i])){
            delete []m_threads;
            throw std::exception();
        }
    }
    
}
template <typename T>
threadpool<T>::~threadpool()
{
    // 释放id空间即可，脱离线程会自行释放资源
    delete []m_threads;
}
//添加读写事件，要改变m_state
template <typename T>
bool threadpool<T>::append(T *request, int state){
    // 操作工作队列时一定要加锁，因为它被所有线程共享。
    m_queuelocker.lock();
    // 判断请求队列是否大于最大请求个数
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    // 设置HTTP请求状态
    request->m_state = state;
    // 向工作队列中添加任务
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    // 通过信号量提示有任务要处理
    m_queuestat.post();
    return true;
}
//添加读完成事件
template <typename T>
bool threadpool<T>::append_p(T *request){
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
//线程回调函数/工作函数，arg其实是this
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    // 将参数强制转为线程池类，调用成员方法
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}
//回调函数会调用这个函数工作
//工作线程就是不断地等任务队列有新任务，然后就加锁取任务->取到任务解锁->执行任务
template <typename T>
void threadpool<T>::run()
{
    // 工作线程从请求队列中取出某个任务进行处理
    while (true)
    {
        //信号量等待
        m_queuestat.wait();
        //唤醒后先加互斥锁
        m_queuelocker.lock();
        if(m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        // 取第一个任务后互斥锁解锁
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;
        // 模式1表示reactor
        if (1 == m_actor_model){
            // 读请求
            if (0 == request->m_state)
            {
                // 读完缓存区内容或用户关闭连接，1表示缓存区正常读完
                if (request->read_once())
                {
                    // 设置improv
                    request->improv = 1;

                    // 从连接池中取出一个数据库连接
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    
                    // 处理http请求的入口
                    request->process();
                }
                // 未读完
                else
                {
                    request->improv = 1;
                    // 计时器标志
                    request->timer_flag = 1;
                }
            }
            //写请求
            else
            {
                // 写请求
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        // 模式0表示proactor
        else{
            connectionRAII mysqlconn(&request->mysql, m_connPool);
            request->process();
        }

    }
    
}


#endif
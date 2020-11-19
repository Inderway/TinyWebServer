#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

//在类名前声明模板
template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量
    max_requests是请求队列中最多允许的、等待处理的请求的数量
    connPool为数据库连接池指针*/
    //线程池中默认8个线程
    //最大请求数默认为8
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    //向请求队列中插入任务请求
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    
    //m_threads为指向元素类型为pthread_t的数组头的指针
    pthread_t * m_threads;       //描述线程池的数组，其大小为m_thread_number
    
    //并未using namespace std
    std::list<T *> m_workqueue; //请求队列
    
    //来自locker.h
    locker m_queuelocker;       //保护请求队列的互斥锁
    
    //请求队列状态，判断队列中是否有任务
    //来自locker.h
    sem m_queuestat;            //是否有任务需要处理
    
    connection_pool *m_connPool;  //数据库
    
    int m_actor_model;          //模型切换
};

template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    //错误处理
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    //初始化线程池
    m_threads = new pthread_t[m_thread_number];
    //初始化数组失败
    if (!m_threads)
        throw std::exception();
    
    for (int i = 0; i < thread_number; ++i)
    {   
        //将工作线程按要求运行
        //第一个参数为指向线程fd的指针
        //第二个参数为线程属性
        //第三个参数为线程运行函数地址
        //第四个参数为运行函数的参数
        //返回0表示成功
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }

        //线程分离后，不用对工作线程回收
        //pthread_detach将线程改为unjoinable状态，以使线程函数退出时释放资源
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool()
{
    //正常析构，释放内存
    delete[] m_threads;
}

//添加请求
template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    //上锁
    m_queuelocker.lock();
    //队列满了，添加失败
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }

    request->m_state = state;
   //加入请求
    m_workqueue.push_back(request);
    //去锁
    m_queuelocker.unlock();
    //通过信号量提醒处理请求
    m_queuestat.post();
    return true;
}

template <typename T>
bool threadpool<T>::append_p(T *request)
{
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

//工作线程函数
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    //传进来的参数是this，因为是void*类型
    //通过强转变为threadpool*
    //再使用其私有方法run
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        //信号量等待
        m_queuestat.wait();
        //唤醒后加锁
        m_queuelocker.lock();
        //请求队列为空叫醒老子？睡觉去了
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }

        //取队头请求
        T *request = m_workqueue.front();
        //弹出队头，没共享变量的事儿了，去锁
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        //这个请求味儿不对
        if (!request)
            continue;
        
        //以下与庖丁解牛不同，新增actor_model 
        if (1 == m_actor_model){
            if (0 == request->m_state){
                if (request->read_once()){
                    request->improv = 1;
                    //从连接池connPool选择数据库连接
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    //处理请求
                    request->process();
                }else{
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }else{
                if (request->write()){
                    request->improv = 1;
                }else{
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }else{
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}
#endif

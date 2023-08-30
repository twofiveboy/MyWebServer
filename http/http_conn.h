#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

//激发http连接数 最大数量对应于最大fd
class http_conn
{
public:
    static const int FILENAME_LEN = 200;        //设置读取文件的名称m_real_file大小
    static const int READ_BUFFER_SIZE=2048;     //设置读缓冲区m_read_buf大小
    static const int WRITE_BUFFER_SIZE=1024;    //设置写缓冲区m_write_buf大小
    //HTTP报文的请求方法，本项目只用到GET和POST
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    //主状态机的状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    //报文解析的结果
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    //从状态机的状态
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    //初始化套接字地址，函数内部会调用私有方法init
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    //关闭http连接
    void close_conn(bool real_close = true);
    //主从状态机 报文解析
    void process();
    //读取浏览器端发来的全部数据，循环读取客户数据，直到无数据可读或对方关闭连接
    bool read_once();
    //响应报文写入函数
    bool write();
    //获取地址
    sockaddr_in *get_address()
    { 
        return &m_address; 
    }
    //同步线程初始化数据库读取表
    void initmysql_result(connection_pool *connPool);
    //只在Reactor模式下发挥作用
    int timer_flag;     //读写请求是否处理完毕
    int improv;         //读写任务是否成功

private:
    void init();
    //从m_read_buf读取，并处理请求报文
    HTTP_CODE process_read();
    //向m_write_buf写入响应报文数据
    bool process_write(HTTP_CODE ret);
    //主状态机解析报文中的请求行数据
    HTTP_CODE parse_request_line(char *text);
    //主状态机解析报文中的请求头数据
    HTTP_CODE parse_headers(char *text);
    //主状态机解析报文中的请求内容
    HTTP_CODE parse_content(char *text);
    //请求报文响应函数
    HTTP_CODE do_request();

    //m_start_line是已经解析的字符
    //get_line用于将指针向后偏移，指向未处理的字符
    char *get_line(){ return m_read_buf + m_start_line; };
    //从状态机读取一行，分析是请求报文的哪一部分
    LINE_STATUS parse_line();
    //申请IO映射
    void unmap();
    //根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    bool add_response(const char *format, ...); //可变参数
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;       // 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
    static int m_user_count;    // 统计用户的数量
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_sockfd;                       // 当前fd
    sockaddr_in m_address;              // 当前地址
    // 读缓冲区,存储读取的请求报文数据
    char m_read_buf[READ_BUFFER_SIZE];  
    long m_read_idx;                    // 缓冲区中m_read_buf中数据的最后一个字节的下一个位置
    long m_checked_idx;                 // 当前正在分析的字符在读缓冲区中的位置
    int m_start_line;                   // m_read_buf中已经解析的字符个数(当前正在解析的行的起始位置

    // 写缓冲区,存储发出的响应报文数据
    char m_write_buf[WRITE_BUFFER_SIZE]; 
    int m_write_idx;                     // 指示buffer中的长度

    // 主状态机状态
    CHECK_STATE m_check_state;
    //请求方法           
    METHOD m_method;                     


    //以下为解析请求报文中对应的6个变量
    char *m_url;                       // 客户请求的目标文件的文件名
    char m_real_file[FILENAME_LEN];    //客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录
    char *m_version;                   // HTTP协议版本号，我们仅支持HTTP1.1 
    char *m_host;                      // 主机名 
    int m_content_length;              // HTTP请求的消息总长度
    bool m_linger;                     // HTTP请求是否要求保持连接

    char *m_file_address;       //客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;    //目标文件状态，通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];       //io向量机制iovec，我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
    int m_iv_count;         
    int cgi;                    //是否启用的POST
    char *m_string;             //存储请求体数据
    int bytes_to_send;          //剩余发送字节数
    int bytes_have_send;        //已发送字节数
    char *doc_root;             

    map<string, string> m_users;
    int m_TRIGMode;             // 触发模式
    int m_close_log;              

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};




#endif
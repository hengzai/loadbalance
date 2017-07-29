
#include<iostream>
#include<cstdio>
#include<cstdlib>
#include<cstring>
#include<unistd.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<pthread.h>
#include<sys/epoll.h>
#include<assert.h>
#include<semaphore.h>
#include<exception>
#include<list>
#define MAXFD 1000

static int ser_fd[3];//访问服务器

class sem{
public:
	sem(){
		if(sem_init(&m_sem,0,0) != 0){
			perror("sem start error");
		}
	}
	~sem(){
		sem_destroy(&m_sem);
	}
	bool wait(){
		return sem_wait(&m_sem) == 0;
	}
	bool post(){
		return sem_post(&m_sem) == 0;
	}
private:
	sem_t m_sem;
};

class locker{
public:
	locker(){
		if(pthread_mutex_init(&m_mutex,NULL) != 0){
			perror("locker start error");
		}
	}
	~locker(){
		pthread_mutex_destroy(&m_mutex);
	}
	bool lock(){
		return pthread_mutex_lock(&m_mutex);
	}
	bool unlock(){
		return pthread_mutex_unlock(&m_mutex);
	}
private:
	pthread_mutex_t m_mutex;
};	

class threadpool{
private:
	int m_thread_nmb;
	locker m_locker;
	sem m_stat;
	bool m_stop = false;
	std::list<int> m_work;
	int epfd;
	struct epoll_event *fds;
public:
	pthread_t* m_threads = NULL;

private:
	static void* worker(void *args);
	static void* r_process(void *args);
	void run();
public:
	threadpool(int thread_nmb = 2):m_thread_nmb(thread_nmb){
		epfd = epoll_create(MAXFD);
		assert(epfd != -1);
		fds = new struct epoll_event[MAXFD];
		
		if(thread_nmb <= 1){
			throw std::exception();
		}
		m_threads = new pthread_t[m_thread_nmb];
		if(!m_threads){
			throw std::exception();
		}
		int i = 0;
		if(pthread_create(m_threads+i,NULL,worker,this)!= 0){
			delete [] m_threads;
			throw std::exception();	
		}
		for(i = 1;i < thread_nmb;++i){
			printf("create the %dth thread\n",i);
			if(pthread_create(m_threads+i,NULL,r_process,this)!= 0){
				delete [] m_threads;
				throw std::exception();
			}
			if(pthread_detach(m_threads[i])){
				delete [] m_threads;
				throw std::exception();
			}
		}
	}
	bool append(int request);
	void fds_add_in(int epfd,int fd);
	void fds_add_out(int epfd,int fd);
	void fds_del(int epfd,int fd);
	void process();
	bool judge_fd(int n,int fd);
	~threadpool(){
		delete[] m_threads;
		m_stop = true;
	}
};

static bool add_ser(int fd1,int fd2,int fd3){
	ser_fd[0] = fd1;
	ser_fd[1] = fd2;
	ser_fd[2] = fd3;
	return true;
}

bool threadpool::append(int request){
	m_locker.lock();
	m_work.push_back(request);
	m_locker.unlock();
	m_stat.post();
	return true;
}

void threadpool::fds_add_in(int epfd,int fd){
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = fd;
	if(epoll_ctl(epfd,EPOLL_CTL_ADD,fd,&ev) == -1){
		perror("epoll_add error");
	}
}

void threadpool::fds_add_out(int epfd,int fd){
	struct epoll_event ev;
	ev.events = EPOLLOUT;
	ev.data.fd = fd;
	if(epoll_ctl(epfd,EPOLL_CTL_ADD,fd,&ev) == -1){
		perror("epoll_add error");
	}
}

void threadpool::fds_del(int epfd,int fd){
	if(epoll_ctl(epfd,EPOLL_CTL_DEL,fd,NULL) == -1){
		perror("epoll_del error");
	}
}

void* threadpool::worker(void *args){
	threadpool *pool = (threadpool*)args;
	pool->run();
	return pool;
}

void* threadpool::r_process(void *args){
	threadpool *pool = (threadpool*)args;
	pool->process();
	return pool;
}

void threadpool::run(){
	while(!m_stop){
		m_stat.wait();
		m_locker.lock();
		if(m_work.empty()){
			m_locker.unlock();
			continue;
		}
		int fd = m_work.front();
		m_work.pop_front();
		m_locker.unlock();
		if(fd <= 0){
			continue;
		}
		fds_add_in(epfd,fd);
	}
}

static int a = -1;
static int b = -1;
static int c = -1;
static int back_ser_fd(){
	while(1){
		if(a == -1){
			a++;
			return ser_fd[0];
		}
		else if(b == -1){
			b++;
			return ser_fd[1];
		}
		else if(c == -1){
			c++;
			return ser_fd[2];
		}
		else{		
			a = -1;
			b = -1;
			c = -1;
		}
	}
	return -1;
}

bool threadpool::judge_fd(int n,int fd){
	for(int i = 0;i < n;++i){
		if(fds[i].data.fd == fd){
			return true;
		}
	}
	return false;
}

void threadpool::process(){
	while(1){
		int n = epoll_wait(epfd,fds,MAXFD,-1);
		if(n == -1){perror("epoll wait error");}
		else if(n == 0){perror("time out");}
		else{
			int i = 0;
			for(;i < n;++i){
				if(fds[i].events & EPOLLIN){
					char buff[255];
					memset(buff,0,255);
					int tmp;
					tmp = recv(fds[i].data.fd,buff,255,0);
					if(tmp <= 0){
						m_locker.lock();
						if(!judge_fd(n,fds[i].data.fd)){
							m_locker.unlock();
							break;
						}
						fds_del(epfd,fds[i].data.fd);
						m_locker.unlock();
						close(fds[i].data.fd);
						printf("one voer\n");
					}
					m_locker.lock();
					int res = back_ser_fd();
					if(res == -1){
						printf("error\n");
					}
					send(res,buff,tmp,0);
					m_locker.unlock();
				}

				if(fds[i].events & EPOLLOUT){
					char buff[255];
					memset(buff,0,255);
					int tmp;
					tmp = recv(fds[i].data.fd,buff,255,0);
					if(tmp <= 0){
						m_locker.lock();
						if(!judge_fd(n,fds[i].data.fd)){
							m_locker.unlock();
							break;
						}
						fds_del(epfd,fds[i].data.fd);
						m_locker.unlock();
						close(fds[i].data.fd);
						printf("one voer\n");
					}
					m_locker.lock();
					int res = back_ser_fd();
					if(res == -1){
						printf("error\n");//加安全检查
					}
					send(res,buff,tmp,0);
					m_locker.unlock();
				}
			}
		}
	}
	pthread_exit(NULL);
}

int my_bind(){
	int sockfd = socket(AF_INET,SOCK_STREAM,0);
	assert(sockfd != -1);

	struct sockaddr_in saddr;
	memset(&saddr,0,sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(1200);
	saddr.sin_addr.s_addr = inet_addr("127.0.0.1");

	int res = bind(sockfd,(struct sockaddr*)&saddr,sizeof(saddr));
	assert(res != -1);

	printf("bind ok\n");
	return sockfd;
}

int conser1(){
	int sockfd = socket(AF_INET,SOCK_STREAM,0);
	assert(sockfd != -1);

	struct sockaddr_in saddr;
	memset(&saddr,0,sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(1500);
	saddr.sin_addr.s_addr = inet_addr("127.0.0.1");

	int res = connect(sockfd,(struct sockaddr*)&saddr,sizeof(saddr));
	if(res == -1){
		perror("链接服务器出错");
	};

	return sockfd;
}

int conser2(){
	int sockfd = socket(AF_INET,SOCK_STREAM,0);
	assert(sockfd != -1);

	struct sockaddr_in saddr;
	memset(&saddr,0,sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(1600);
	saddr.sin_addr.s_addr = inet_addr("127.0.0.1");

	int res = connect(sockfd,(struct sockaddr*)&saddr,sizeof(saddr));
	if(res == -1){
		perror("链接服务器出错");
	};

	return sockfd;
}

int conser3(){
	int sockfd = socket(AF_INET,SOCK_STREAM,0);
	assert(sockfd != -1);

	struct sockaddr_in saddr;
	memset(&saddr,0,sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(1700);
	saddr.sin_addr.s_addr = inet_addr("127.0.0.1");

	int res = connect(sockfd,(struct sockaddr*)&saddr,sizeof(saddr));
	if(res == -1){
		perror("链接服务器出错");
	};

	return sockfd;
}

int main(){
	int b_fd = my_bind();
	int ser1 = conser1();
	int ser2 = conser2();
	int ser3 = conser3();
	threadpool pth(4);
	add_ser(ser1,ser2,ser3);
	int num = 0;
		
	struct sockaddr_in saddr;
	memset(&saddr,0,sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(1700);
	saddr.sin_addr.s_addr = inet_addr("127.0.0.1");
	

	listen(b_fd,5);
	
	socklen_t len = sizeof(saddr);
	while(1){
		sleep(1);
		int c = accept(b_fd,(struct sockaddr*)&saddr,&len);
		printf("already   connet %d  \n",++num);
		pth.append(c);
	}
	return 0;

}


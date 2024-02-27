///////////////////////////////////////////////////////////////////////
// File Name : web3_3_C_2020202004.c			     	     //
// Date : 2023/05/24						     //
// Os : Ubuntu 16.04 LTS 64bits					     //
// Author : Ryu Young Jun					     //
// Student ID : 2020202004					     //
// ----------------------------------------------------------------- //
// Title : System Programming Assignment #3-3 (semaphore_server)     //
// Description : This program prints log to terminal, file. Implemen //
// -ted to make log file and use binary semaphore whenever it access //
// -es log file. Moreover, connection history doesn't printed to log //
// file, only terminal.						     //
///////////////////////////////////////////////////////////////////////

#define _GNU_SOURCE
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <pthread.h>
#include <fnmatch.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/types.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>


///////////////////////////////////////////////////////////////////////
// SWAP								     //
// ================================================================= //
// Input: type -> type of variables to be swapped		     //
//		  a	   -> swap target 1			     //
//		  b	   -> swap target 2			     //
//								     //
// Output: void							     //
//								     //
// Purpose: swap variables of any type				     //
///////////////////////////////////////////////////////////////////////
#define SWAP(type, a, b) { type temp = a; a = b; b = temp; }
#define IP "127.0.0.1"
#define PORT 40000
#define MEMSIZE 8192
#define HISTORY_OFFSET 4096

struct connectInfo{
	int no; //connected order
	char ipaddr[20]; //client ip
	pid_t pid; //pid
	int port; //client port
	time_t connectedTime; //connected time
};

struct procInfo{ //process block managed in the process pool
	pid_t pid;
	char idleFlag;
};

struct argProc{ //argument packed for process pool
	pid_t pid;
	void *shm_addr;
	char parentSigFlag; //if 1, send signal to parent process
};

struct argHistory{ //argument packed for history
	void *shm_addr;
	char ipaddr[20]; //client ip
	pid_t pid; //pid
	int port; //client port
	time_t connectedTime; //connected time
};

struct retProcCnt{ //return value packed for getProcCntPool()
	int totalProcCnt; //total proccess count in the process pool
	int idleProcCnt; //idle proccess count in the process pool
};


char whitelistFlag = 0, *whitelistBuf = NULL, originWD[512]; //variables associated with accessible.usr file
int serv_sock, shm_id, logFd; //server socket fd, shared memory id, logfile desc.
int maxChild, maxIdle, minIdle, startProc, maxHistory; //httpd.conf values
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; //mutex for synchronization
void *shm_addr; //shared memory address


//process pool management
void* initProcPool(void *);
void* addIdleProcPool(void *);
void* setBusyProcPool(void *);
void* delIdleProcPool(void *);
void* getProcCntPool(void *);
void manageProcPool(int);

//terminate server
void* cleanUpPool(void *);
void exitChildHandler(int);
void exitChild(int);

//history management
void* addHistory(void *);
void* printHistory(void *);
void printHistoryHandler(int);

//service
void serviceMain();
void* printLog(void *);
char chkWhiteList(char *, char *);
void sendResponse(char *, int, char *, char *, int, int);
long int getNameListDir(DIR *, char **, int *, char);
void printFileAttr(char *, char *, char *);
char* upStr(char *);
char nameCmp(char **, char **);


int main(int argc, char *argv[]){
	struct stat wlStat, conf;
	struct sockaddr_in serv_addr; //address info struct
	int optval = 1, i;
	time_t t;
	pthread_t tid; //thread id
	char timebuf[50], *confBuf, *confVal, logBuf[64];

	if((shm_id = shmget((key_t)PORT, MEMSIZE, IPC_CREAT | 0666)) == -1){ //get shared memory segment
		perror("shmget() main()");
		exit(EXIT_FAILURE);
	}
	if((shm_addr = shmat(shm_id, (void *)0, 0)) == (void*)-1){ //attach the segment to process
		perror("shmat() main()");
		exit(EXIT_FAILURE);
	}
	pthread_create(&tid, NULL, &initProcPool, shm_addr); //initialize the idle process count
	pthread_join(tid, NULL); //wait until the thread terminates

	if(lstat("httpd.conf", &conf) == -1){ //read httpd.conf file
		perror("lstat(\"httpd.conf\") error");
		exit(EXIT_FAILURE);
	}

	confBuf = malloc(conf.st_size);
	int configFd = open("httpd.conf", O_RDONLY); //open httpd.conf
	read(configFd, confBuf, conf.st_size); //get configuration
	close(configFd); //close httpd.conf

	strtok(confBuf, "\n"); //[httpd.conf]
	confVal = strtok(NULL, "\n"); //MaxChilds
	if(!fnmatch("MaxChilds:*", confVal, 0))
		maxChild = atoi(confVal + strlen("MaxChilds:"));
	confVal = strtok(NULL, "\n"); //MaxIdleNum
	if(!fnmatch("MaxIdleNum:*", confVal, 0))
		maxIdle = atoi(confVal + strlen("MaxIdleNum:"));
	confVal = strtok(NULL, "\n"); //MinIdleNum
	if(!fnmatch("MinIdleNum:*", confVal, 0))
		minIdle = atoi(confVal + strlen("MinIdleNum:"));
	confVal = strtok(NULL, "\n"); //StartServers
	if(!fnmatch("StartServers:*", confVal, 0))
		startProc = atoi(confVal + strlen("StartServers:"));
	confVal = strtok(NULL, "\n"); //MaxHistory
	if(!fnmatch("MaxHistory:*", confVal, 0))
		maxHistory = atoi(confVal + strlen("MaxHistory:"));

	logFd = open("server_log.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666); //log file open
	sem_t *logSem = sem_open("LOGSEM", O_CREAT | O_EXCL, 0700, 1); //get binary semaphore
	sem_close(logSem);

	time(&t); //get current time
	strftime(timebuf, 50, "%c", gmtime(&t)); //change current time to formatted string
	
	sprintf(logBuf, "[%s] Server is started.\n\n", timebuf);
	fputs(logBuf, stdout); //print to terminal
	pthread_create(&tid, NULL, printLog, (void *)logBuf); //print to log file
	pthread_join(tid, NULL);

	signal(SIGUSR2, manageProcPool); //set SIGUSR2 handler to manageProcPool
	signal(SIGINT, exitChild); //set SIGINT signal handler
	signal(SIGALRM, printHistoryHandler); //set SIGALRM signal handler
	alarm(10); //start timer

	if(lstat("accessible.usr", &wlStat) != -1){ //check if whitelist exists
		whitelistFlag = 1;
		whitelistBuf = malloc(wlStat.st_size); //allocate buffer of whitelist size
		int wlFd = open("accessible.usr", O_RDONLY); //open whitelist
		read(wlFd, whitelistBuf, wlStat.st_size); //get whitelist info
		close(wlFd); //close
	}

	getcwd(originWD, 512); //get initial working directory

	if((serv_sock = socket(PF_INET, SOCK_STREAM, 0)) == -1){ //open server socket
		perror("socket() error");
		exit(EXIT_FAILURE);
	}

	if(setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1){ //prevent binding timeout error
		perror("setsockopt()");
		exit(EXIT_FAILURE);
	}

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET; //set protocol family
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY); //set address to server's
	serv_addr.sin_port = htons(PORT); //set port number to 40000
	if(bind(serv_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1){ //bind socket with information
		perror("bind() error");
		exit(EXIT_FAILURE);
	}

	if(listen(serv_sock, 5) == -1){ //wait the client's request
		perror("listen() error");
		exit(EXIT_FAILURE);
	}

	for(i = 0; i < startProc; i++){
		pid_t pid;
		if((pid = fork()) > 0){ //make new child process
			time(&t); //get current time
			strftime(timebuf, 50, "%c", gmtime(&t)); //get formatted string using current time
		
			sprintf(logBuf, "[%s] %d process is forked.\n", timebuf, pid);
			fputs(logBuf, stdout); //print to terminal
			pthread_create(&tid, NULL, printLog, (void *)logBuf); //print to log file
			pthread_join(tid, NULL);

			struct argProc argProcess = {pid, shm_addr, 0}; //argument to pass to thread
			pthread_create(&tid, NULL, &addIdleProcPool, (void *)&argProcess); //increase idle process count
			pthread_join(tid, NULL); //wait until the thread terminates
		}
		else
			serviceMain(); //if child process, run the actual communication logic
	}
	printf("\n"); //print to terminal
	strcpy(logBuf, "\n"); //print to log file
	pthread_create(&tid, NULL, printLog, (void *)logBuf);
	pthread_join(tid, NULL);

	while(1)
		pause(); //pause parent process until get signal
}


///////////////////////////////////////////////////////////////////////
// serviceMain							     //
// ================================================================= //
// Input: void							     //	
//								     //
// Output: void							     //
//								     //
// Purpose: to communicate with client. actual logic the child runs. //
///////////////////////////////////////////////////////////////////////
void serviceMain(){
	int connectedCnt = 0, shm_id;
	void* shm_addr;
	signal(SIGINT, SIG_IGN); //ignore the interrupt signal
	signal(SIGALRM, SIG_IGN); //ignore the alarm signal
	signal(SIGTERM, exitChildHandler); //set signal hander for termination signal

	if((shm_id = shmget((key_t)PORT, MEMSIZE, IPC_CREAT | 0666)) == -1){ //get shared memory segment
		perror("shmget() serviceMain()");
		exit(EXIT_FAILURE);
	}
	if((shm_addr = shmat(shm_id, (void*)0, 0)) == (void *)-1){ //attach shared memory segment to this process
		perror("shmat() serviceMain()");
		exit(EXIT_FAILURE);
	}

	while(1){
		pthread_t tid;
		struct sockaddr_in clnt_addr;
		struct argHistory argConnect;
		struct timeval connectTime, disconnectTime;
		time_t currentTime;
		int i, j, entryCnt, totalBlk, readSz, clnt_sock, clnt_addr_sz = sizeof(clnt_addr);
		char requestHeader[2048] = {0, }, responseData[100000] = {0, }, temp[8192], *nameList[512], path[512], urlTemp[512], timebuf[50], logBuf[256];
		char notExistFlag = 0, fileFlag = 0, *url, *clnt_ip;
		DIR *dp;

		if((clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_sz)) == -1){ //accept the request of client
			perror("accept() error");
			exit(EXIT_FAILURE);
		}
		gettimeofday(&connectTime, NULL); //get higher resolution time
		time(&currentTime); //get current time client connected

		clnt_ip = inet_ntoa(clnt_addr.sin_addr); //change the client ip to dotted decimal
		if(whitelistFlag && !chkWhiteList(clnt_ip, whitelistBuf)){ //not accessible
			sprintf(responseData, "<h1>Access denied!</h1><h2>Your IP : %s</h2><br>You have no permission to access this web server.<br>HTTP 403.6 - Forbidden: IP address reject", clnt_ip); //print 403 error
			sendResponse(responseData, 403, "Forbidden", "text/html", strlen(responseData), clnt_sock); //response to the client
			close(clnt_sock); //disconnect
			continue;
		}
		
		struct argProc argProcess = {getpid(), shm_addr, 1};
		pthread_create(&tid, NULL, &setBusyProcPool, (void *)&argProcess); //conver process state from idle to busy
		pthread_join(tid, NULL); //wait until the thread terminates
	
		readSz = read(clnt_sock, requestHeader, 2048); //read HTTP request from client
		if(readSz == -1){
			perror("read() error");
			exit(EXIT_FAILURE);
		}
		else if(readSz == 0){ //close connection
			close(clnt_sock);
			
			argProcess.pid = getpid();
			argProcess.shm_addr = shm_addr;
			argProcess.parentSigFlag = 1; //after the process is changed to idle, parent will check the idle process count
			pthread_create(&tid, NULL, &addIdleProcPool, (void *)&argProcess); //change the process state from busy to idle
			pthread_join(tid, NULL);
			continue;
		}

		url = strtok(requestHeader, " ");
		while(url && strcmp(url, "GET")) //if token is not GET
			url = strtok(NULL, " "); //get next token
		if(url == NULL){ //if token is not GET, print error and exit
			fputs("Failed to get path from request\n", stderr);
			exit(EXIT_FAILURE);
		}
	
		url = strtok(NULL, " "); //get url path
		strcpy(urlTemp, url);
		if(url[strlen(url) - 1] == '/') //if url is terminated with '/'
			url[strlen(url) - 1] = 0; //get rid of '/'

		strcpy(path, originWD);
		strcat(path, url);
	
		strftime(timebuf, 50, "%c", gmtime(&currentTime));
		sprintf(logBuf, "=========== New Client ==========\nTIME: [%s]\nURL : %s\nIP : %s\nPort : %d\nPID : %d\n=================================\n\n", timebuf, urlTemp, clnt_ip, ntohs(clnt_addr.sin_port), getpid());
		fputs(logBuf, stdout); //print to terminal
		pthread_create(&tid, NULL, printLog, (void *)logBuf); //print to log file
		pthread_join(tid, NULL);
		
		//set client access information
		argConnect.shm_addr = shm_addr; //shared memory address
		strcpy(argConnect.ipaddr, clnt_ip); //client ip
		argConnect.pid = getpid(); //store current process pid
		argConnect.port = ntohs(clnt_addr.sin_port); //client port
		argConnect.connectedTime = currentTime; //connected time

		pthread_create(&tid, NULL, &addHistory, (void *)&argConnect); //add connection history to shared memory
		pthread_join(tid, NULL);
		
		if((dp = opendir(path)) == NULL){
			if(errno == ENOENT){
				sprintf(responseData, "<h1>Not Found</h1><br>The URL %s was not found on this server<br>HTTP 404 - Not Page Found", path); //print 404 error
				sendResponse(responseData, 404, "Not Found", "text/html", strlen(responseData), clnt_sock); //response to the client
				close(clnt_sock); //disconnect
				gettimeofday(&disconnectTime, NULL); //get higher resolution time
				time(&currentTime); //get cuurent time client disconnected
				strftime(timebuf, 50, "%c", gmtime(&currentTime));
				
				sprintf(logBuf, "====== Disconnected Client ======\nTIME: [%s]\nURL : %s\nIP : %s\nPort : %d\nPID : %d\nCONNECTING TIME : %ld(us)\n=================================\n\n", timebuf, urlTemp, clnt_ip, ntohs(clnt_addr.sin_port), getpid(), 1000000 * (disconnectTime.tv_sec - connectTime.tv_sec) + disconnectTime.tv_usec - connectTime.tv_usec);
				fputs(logBuf, stdout); //print to terminal
				pthread_create(&tid, NULL, printLog, (void *)logBuf); //print to log file
				pthread_join(tid, NULL);

				sleep(5); //5second sleep
				argProcess.pid = getpid();
				argProcess.shm_addr = shm_addr; //pass shared memory address
				argProcess.parentSigFlag = 1; //after the thread works, send signal to parent to manage pool
				pthread_create(&tid, NULL, &addIdleProcPool, shm_addr);
				pthread_join(tid, NULL);
				continue;
			}
			else if(errno != ENOTDIR){ //if unknown error, terminate program
				perror("opendir(url) error");
				exit(EXIT_FAILURE);
			}
			else fileFlag = 1; //set file flag
		}

		if(fileFlag){ //if file is non-directory file
			char urlCopy[512], contentType[16], *contentBuf, *bname;
			int contentFd;
			struct stat contentAttr;

			strcpy(urlCopy, path);
			bname = basename(urlCopy); //get file name
				
			if(fnmatch("*.jpg", bname, FNM_CASEFOLD) || fnmatch("*.jpeg", bname, FNM_CASEFOLD) || fnmatch("*.png", bname, FNM_CASEFOLD)) //if file is image
				strcpy(contentType, "image/*");
			else if(fnmatch("*.html", bname, FNM_CASEFOLD)) //if file is html
				strcpy(contentType, "text/html");
			else strcpy(contentType, "text/plain"); //if file is plain text

			if(lstat(path, &contentAttr) == -1){ //get file properties
				perror("lstat() error from contentAttr");
				exit(EXIT_FAILURE);
			}

			if((contentFd = open(path, O_RDONLY)) == -1){ //open file
				perror("open() error from contentFd");
				exit(EXIT_FAILURE);
			}

			contentBuf = malloc(contentAttr.st_size); //allocate buffer of file size
			int sz = read(contentFd, contentBuf, contentAttr.st_size); //get file data
			sendResponse(contentBuf, 200, "OK", contentType, contentAttr.st_size, clnt_sock); //send file data to client
			
			free(contentBuf); //deallocate
			close(contentFd); //close file
			close(clnt_sock); //disconnect
			gettimeofday(&disconnectTime, NULL); //get higher resolution time
			time(&currentTime); //get current time client disconnected
			strftime(timebuf, 50, "%c", gmtime(&currentTime));
			
			sprintf(logBuf, "====== Disconnected Client ======\nTIME: [%s]\nURL : %s\nIP : %s\nPort : %d\nPID : %d\nCONNECTING TIME : %ld(us)\n=================================\n\n", timebuf, urlTemp, clnt_ip, ntohs(clnt_addr.sin_port), getpid(), 1000000 * (disconnectTime.tv_sec - connectTime.tv_sec) + disconnectTime.tv_usec - connectTime.tv_usec);
			fputs(logBuf, stdout); //print to terminal
			pthread_create(&tid, NULL, printLog, (void *)logBuf); //print to log file
			pthread_join(tid, NULL);

			sleep(5); //5second sleep
			argProcess.pid = getpid();
		        argProcess.shm_addr = shm_addr;
		      	argProcess.parentSigFlag = 1; //after the process state is change to idle, parent will check the idle process count
			pthread_create(&tid, NULL, &addIdleProcPool, (void *)&argProcess); //change the process state from busy to idle
			pthread_join(tid, NULL);
			continue;
		}

		sprintf(temp, "<h1>%sSystem Programming HTTP</h1><br>\n <b>Directory path : %s</b><br>\n", !strcmp(path, originWD) ? "Welcome to " : "", path); //print title
		strcat(responseData, temp); //accumulate to the buffer
		
		chdir(path); //change current workinng directory to url
		totalBlk = getNameListDir(dp, nameList, &entryCnt, !strcmp(originWD, path) ? 0 : 1); //get file name list from working directory and sum of 1K block
		for(i = 0; i < entryCnt - 1; i++) //bubble sort the name list to ascending order
			for(j = 0; j < entryCnt - 1 - i; j++)
				nameCmp(&nameList[j], &nameList[j + 1]); //compare adjacent string
		
		//print file table
		sprintf(temp, "<b>total : %d</b><br>\n<table border=\"1\">\n<tr>\n<th>Name</th>\n<th>Permission</th>\n<th>Link</th>\n<th>Owner</th>\n<th>Group</th>\n<th>Size</th>\n<th>Last Modified</th>\n</tr>\n", totalBlk);
		strcat(responseData, temp); //accumulate data
		for(i = 0; i < entryCnt; i++)
			printFileAttr(nameList[i], url, responseData); //print properties of the child files
		strcat(responseData, "</table>\n"); //end of table
			
		sendResponse(responseData, 200, "OK", "text/html", strlen(responseData), clnt_sock); //send response with data of direcoty file table
		closedir(dp); //close directory stream
		close(clnt_sock); //disconnect
		gettimeofday(&disconnectTime, NULL); //get higher resolution time
		time(&currentTime);
		strftime(timebuf, 50, "%c", gmtime(&currentTime));
		
		sprintf(logBuf, "====== Disconnected Client ======\nTIME: [%s]\nURL : %s\nIP : %s\nPort : %d\nPID : %d\nCONNECTING TIME : %ld(us)\n=================================\n\n", timebuf, urlTemp, clnt_ip, ntohs(clnt_addr.sin_port), getpid(), 1000000 * (disconnectTime.tv_sec - connectTime.tv_sec) + disconnectTime.tv_usec - connectTime.tv_usec);
		fputs(logBuf, stdout); //print to terminal
		pthread_create(&tid, NULL, printLog, (void *)logBuf); //print to log file
		pthread_join(tid, NULL);
		
		chdir(originWD); //restore the working directory to origin
		
		sleep(5); //5second sleep
		argProcess.pid = getpid();
		argProcess.shm_addr = shm_addr;
	       	argProcess.parentSigFlag = 1; //after the process state is change to idle, parent will check the idle process count
		pthread_create(&tid, NULL, &addIdleProcPool, (void *)&argProcess); //change the process state from busy to idle
		pthread_join(tid, NULL);
	}
}


///////////////////////////////////////////////////////////////////////
// initProcPool							     //
// ================================================================= //
// Input: void* -> shared memory address			     //
//								     //
// Output: void*						     //
//								     //
// Purpose: thread handler initializing shared memory segment	     //
///////////////////////////////////////////////////////////////////////
void* initProcPool(void *shm_addr){
	pthread_mutex_lock(&mutex);
	memset(shm_addr, 0, MEMSIZE); //initialize shared memory segment to 0
	pthread_mutex_unlock(&mutex);
	return NULL;
}


///////////////////////////////////////////////////////////////////////
// addIdleProcPool						     //
// ================================================================= //
// Input: void* -> struct argProc pointer			     //
//								     //
// Output: void*						     //
//								     //
// Purpose: thread handler that adds idle proccess		     //
///////////////////////////////////////////////////////////////////////
void* addIdleProcPool(void *arg){
	void *shm_addr = ((struct argProc *)arg)->shm_addr; //get shared memory address from arg
	void *table = shm_addr + 2 * sizeof(int); //calculate start address of process list
	int totalProcCnt = 0, idleProcCnt = 0, offset = 0, emptyOffset = -1, i = 0;
	time_t t;
	char timebuf[50], existFlag = 0, logBuf[40];
	pthread_t tid;
       
	pthread_mutex_lock(&mutex);
	memcpy(&totalProcCnt, shm_addr, sizeof(int)); //get current total proc count
	memcpy(&idleProcCnt, shm_addr + sizeof(int), sizeof(int)); //get current idle proc count
	while(i < totalProcCnt){ //move offset next to set idle proc until the last valid entry
		struct procInfo *p = (struct procInfo *)(table + offset * sizeof(struct procInfo));
		if(p->pid == ((struct argProc *)arg)->pid){ //struct for process already exists
			p->idleFlag = 1; //set proc to idle
			existFlag = 1;
			break;
		}
		if(p->pid == -1) //if empty entry, record that entry
			emptyOffset = offset;
		else i++;
		offset++; //move offset
	}
	if(!existFlag){ //if the procces is new
		struct procInfo *p = (struct procInfo *)(table + ((emptyOffset != -1) ? emptyOffset : offset) * sizeof(struct procInfo));
		p->pid = ((struct argProc *)arg)->pid;
		p->idleFlag = 1; //set proc to idle
		totalProcCnt++; //increase proccess count
		memcpy(shm_addr, &totalProcCnt, sizeof(int));
	}

	idleProcCnt++;
	memcpy(shm_addr + sizeof(int), &idleProcCnt, sizeof(int)); //increase idle proccess count
	pthread_mutex_unlock(&mutex);

	time(&t);
	strftime(timebuf, 50, "%c", gmtime(&t));

	sprintf(logBuf, "[%s] IdleProcessCount : %d\n", timebuf, idleProcCnt);
	fputs(logBuf, stdout);
	pthread_create(&tid, NULL, printLog, (void *)logBuf);
	pthread_join(tid, NULL);
	
	if(((struct argProc *)arg)->parentSigFlag) //if signal flag is set
		kill(getppid(), SIGUSR2); //signal to parent process to get rid of or create some process if need
}


///////////////////////////////////////////////////////////////////////
// setBusyProcPool						     //
// ================================================================= //
// Input: void* -> struct argProc ponter			     //
//								     //
// Output: void*						     //
//								     //
// Purpose: thread handler that changes state from idle to busy	     //
///////////////////////////////////////////////////////////////////////
void* setBusyProcPool(void *arg){
	void *table = ((struct argProc *)arg)->shm_addr + 2 * sizeof(int); //table start address
	int totalProcCnt = 0, idleProcCnt = 0, i = 0, offset = 0;
	time_t t;
	char timebuf[50], logBuf[40];
	pthread_t tid;

	pthread_mutex_lock(&mutex);
	memcpy(&totalProcCnt, shm_addr, sizeof(int)); //get total process count
	memcpy(&idleProcCnt, shm_addr + sizeof(int), sizeof(int)); //get idle process count

	while(i < totalProcCnt){ //linear search of all proccess
		struct procInfo *p = (struct procInfo *)(table + offset * sizeof(struct procInfo));

		if(p->pid == ((struct argProc *)arg)->pid){ //found process in the list
			p->idleFlag = 0; //set proccess from idle to busy
			idleProcCnt--;
			memcpy(shm_addr + sizeof(int), &idleProcCnt, sizeof(int)); //decrease idle process count
			pthread_mutex_unlock(&mutex);
			
			time(&t);
			strftime(timebuf, 50, "%c", gmtime(&t));
			
			sprintf(logBuf, "[%s] IdleProcessCount : %d\n\n", timebuf, idleProcCnt); //print updated idle process count
			fputs(logBuf, stdout);
			pthread_create(&tid, NULL, printLog, (void *)logBuf);
			pthread_join(tid, NULL);

			kill(getppid(), SIGUSR2); //send SIGUSR2 signal to parent process
			return NULL;
		}
		offset++; //increase to move pointer to next entry
		if(p->pid != -1) i++; //skip empty entry
	}

	//not found procInfo for (struct argProc *)arg->pid
	pthread_mutex_unlock(&mutex);
	return NULL;
}


///////////////////////////////////////////////////////////////////////
// delIdleProcPool						     //
// ================================================================= //
// Input: void* -> shared memory address			     //
//								     //
// Output: void*						     //
//								     //
// Purpose: thread handler that removes process from pool	     //
///////////////////////////////////////////////////////////////////////
void* delIdleProcPool(void *shm_addr){
	time_t t;
	char tbuf[50], logBuf[40];
	int totalProcCnt, idleProcCnt, i = 0, offset = 0;
	void *table = shm_addr + 2 * sizeof(int);
	pthread_t tid;

	pthread_mutex_lock(&mutex);
	memcpy(&totalProcCnt, shm_addr, sizeof(int));
	memcpy(&idleProcCnt, shm_addr + sizeof(int), sizeof(int));

	while(i < totalProcCnt){
		struct procInfo *p = table + offset * sizeof(struct procInfo);
		if(p->pid != -1 && p->idleFlag){
			kill(p->pid, SIGTERM); //terminate child process
			waitpid(p->pid, NULL, 0); //wait until child process is terminated
			time(&t); //get current time
			strftime(tbuf, 50, "%c", gmtime(&t)); //get formatted string using current time

			sprintf(logBuf, "[%s] %d process is terminated.\n", tbuf, p->pid);
			fputs(logBuf, stdout);
			pthread_create(&tid, NULL, printLog, (void *)logBuf);
			pthread_join(tid, NULL);
			p->pid = -1;

			totalProcCnt--; idleProcCnt--; //decrease total/idle proccess count
			memcpy(shm_addr, &totalProcCnt, sizeof(int));
			memcpy(shm_addr + sizeof(int), &idleProcCnt, sizeof(int));
			pthread_mutex_unlock(&mutex);
			
			time(&t); //get current time
			strftime(tbuf, 50, "%c", gmtime(&t)); //get formatted string using current time

			sprintf(logBuf, "[%s] IdleProcessCount : %d\n", tbuf, idleProcCnt);
			fputs(logBuf, stdout);
			pthread_create(&tid, NULL, printLog, (void *)logBuf);
			pthread_join(tid, NULL);
			return NULL;
		}
		offset++;
		if(p->pid != -1) i++;
	}

	//idle process is not found
	pthread_mutex_unlock(&mutex);
	return NULL;
}


///////////////////////////////////////////////////////////////////////
// getProcCntpool						     //
// ================================================================= //
// Input: void* -> shared memory address			     //
//								     //
// Output: void*-> struct retProcCnt pointer			     //
//								     //
// Purpose: thread handler that gets total/idle proccess count	     //
///////////////////////////////////////////////////////////////////////
void* getProcCntPool(void *shm_addr){
	struct retProcCnt *ret = malloc(sizeof(struct retProcCnt));
	int cnt = 0;

	pthread_mutex_lock(&mutex);
	memcpy(&cnt, shm_addr, sizeof(int)); //get total proccess count
	ret->totalProcCnt = cnt;
	memcpy(&cnt, shm_addr + sizeof(int), sizeof(int)); //get idle proccess count
	ret->idleProcCnt = cnt;
	pthread_mutex_unlock(&mutex);
	
	return (void *)ret;
}


///////////////////////////////////////////////////////////////////////
// manageProcPool						     //
// ================================================================= //
// Input: int -> signal number					     //
//								     //
// Output: void						     	     //
//								     //
// Purpose: SIGUSR2 handler. makes or removes some idle processes    //
///////////////////////////////////////////////////////////////////////
void manageProcPool(int sig){
	int i, totalProcCnt, idleProcCnt;
	struct retProcCnt* ret;
	pthread_t tid;
	char logBuf[40];
	
	pthread_create(&tid, NULL, &getProcCntPool, shm_addr); //get idle process count
	pthread_join(tid, (void **)&ret); //get return
	
	totalProcCnt = ret->totalProcCnt;
	free(ret); //free allocated space
	for(i = totalProcCnt; i > maxChild; i--){ //total process count exceeds maxChild
		pthread_create(&tid, NULL, &delIdleProcPool, (void *)shm_addr); //terminate idle process
		pthread_join(tid, NULL);
	}
	
	//get process count onec again
	pthread_create(&tid, NULL, &getProcCntPool, shm_addr); //get idle process count
	pthread_join(tid, (void **)&ret); //get return
	
	totalProcCnt = ret->totalProcCnt;
	idleProcCnt = ret->idleProcCnt;
	free(ret); //free allocated space

	if(idleProcCnt < minIdle){ //have to make more processes
		for(i = idleProcCnt; i < 5 && totalProcCnt + (i - idleProcCnt) < maxChild; i++){ //make idle proc until 5 and lower than or equal with maxChild
			int pid = fork(); //make new child
			if(pid > 0){
				time_t t;
				char timebuf[50];
				time(&t); //get current time
				strftime(timebuf, 50, "%c", gmtime(&t)); //get formatted string from time_t

				sprintf(logBuf, "[%s] %d process is forked.\n", timebuf, pid); //print forked log
				fputs(logBuf, stdout); //print to terminal
				pthread_create(&tid, NULL, printLog, (void *)logBuf); //print to log file
				pthread_join(tid, NULL);

				struct argProc argProcess = {pid, shm_addr, 0};
				pthread_create(&tid, NULL, &addIdleProcPool, (void *)&argProcess); //increase idle process count
				pthread_join(tid, NULL); //wait until the thread terminates
			}
			else
				serviceMain(); //start child's main
		}
		strcpy(logBuf, "\n");
		fputs(logBuf, stdout); //print to terminal
		pthread_create(&tid, NULL, printLog, (void *)logBuf); //print to log file
		pthread_join(tid, NULL);
	}
	else if(idleProcCnt > maxIdle){ //have to terminate some idle processes
		for(i = idleProcCnt; i > 5; i--){
			pthread_create(&tid, NULL, &delIdleProcPool, (void *)shm_addr); //remove idle process
			pthread_join(tid, NULL);
		}
	}
	printf("\n");
}


///////////////////////////////////////////////////////////////////////
// cleanUpPool							     //
// ================================================================= //
// Input: void* -> shared memory address			     //
//								     //
// Output: void*						     //
//								     //
// Purpose: thread handler that terminates all processes in pool     //
///////////////////////////////////////////////////////////////////////
void* cleanUpPool(void *shm_addr){
	int totalProcCnt, idleProcCnt, busyProcCnt, i = 0, offset = 0;
	void *table = shm_addr + 2 * sizeof(int); //table start address
	char tbuf[50], logBuf[200];
	time_t t;
	pthread_t tid;

	strcpy(logBuf, "^C\n"); //print to log file
	pthread_create(&tid, NULL, printLog, (void *)logBuf);
	pthread_join(tid, NULL);
	
	pthread_mutex_lock(&mutex);
	memcpy(&totalProcCnt, shm_addr, sizeof(int)); //get total process count
	memcpy(&idleProcCnt, shm_addr + sizeof(int), sizeof(int)); //get idle process count
	busyProcCnt = totalProcCnt - idleProcCnt;

	//cleanup busy processes first
	while(i < busyProcCnt){
		struct procInfo *p = (struct procInfo *)(table + offset * sizeof(struct procInfo));
		if(p->pid != -1 && p->idleFlag == 0){ //process p is busy process
			kill(p->pid, SIGTERM); //terminate busy process
			waitpid(p->pid, NULL, 0); //wait until child process is terminated

			time(&t);
			strftime(tbuf, 50, "%c", gmtime(&t));
			
			sprintf(logBuf, "[%s] %d process is terminated.\n[%s] BusyProcessCount : %d\n", tbuf, p->pid, tbuf, busyProcCnt - (++i));
			fputs(logBuf, stdout);
			pthread_create(&tid, NULL, printLog, (void *)logBuf);
			pthread_join(tid, NULL);
			
			p->pid = -1; //mark invalid block
		}
		offset++;
	}

	if(busyProcCnt > 0) printf("\n");

	//cleanup idle processes
	offset = 0; i = 0; //reset offset and counter to zero
	while(i < idleProcCnt){
		struct procInfo *p = (struct procInfo *)(table + offset * sizeof(struct procInfo));
		if(p->pid != -1){ //all remained processes are idle processes
			kill(p->pid, SIGTERM); //terminate child process
			waitpid(p->pid, NULL, 0); //wait until child process is terminated
			
			time(&t); //get current time
			strftime(tbuf, 50, "%c", gmtime(&t)); //get formatted string using current time
			sprintf(logBuf, "[%s] %d process is terminated.\n[%s] IdleProcessCount : %d\n", tbuf, p->pid, tbuf, idleProcCnt - (++i));
			fputs(logBuf, stdout); //print to terminal
			pthread_create(&tid, NULL, printLog, (void *)logBuf); //print to log file
			pthread_join(tid, NULL);
		}
		offset++;
	}
	pthread_mutex_unlock(&mutex);
	return NULL;
}


///////////////////////////////////////////////////////////////////////
// exitChild							     //
// ================================================================= //
// Input: int -> signal number				     	     //
//								     //
// Output: void							     //
//								     //
// Purpose: signal handler of SIGINT. kill the child processes before//
//          terminate parent process.				     //
///////////////////////////////////////////////////////////////////////
void exitChild(int signum){
	time_t t;
	char tbuf[50], logBuf[40];
	int i;
	pthread_t tid;

	alarm(0); //alarm off
	printf("\n"); //print to terminal

	pthread_create(&tid, NULL, &cleanUpPool, (void *)shm_addr); //process pool clean up
	pthread_join(tid, NULL);
	shmctl(shm_id, IPC_RMID, NULL); //remove shared memory segment
	pthread_mutex_destroy(&mutex);

	time(&t); //get current time
	strftime(tbuf, 50, "%c", gmtime(&t)); //get fromatted string using current time
	
	sprintf(logBuf, "\n[%s] Server is terminated.\n", tbuf); //print to terminal
	fputs(logBuf, stdout); //print to log file
	pthread_create(&tid, NULL, printLog, (void *)logBuf);
	pthread_join(tid, NULL);

	close(logFd); //close log file
	sem_unlink("LOGSEM"); //remove semaphore
	exit(EXIT_SUCCESS); //termiate parent process
}


///////////////////////////////////////////////////////////////////////
// exitChildHandler						     //
// ================================================================= //
// Input: int -> signal number					     //
//								     //
// Output: void							     //
//								     //
// Purpose: SIGTERM handler. Detaches shared memory before exit      //
///////////////////////////////////////////////////////////////////////
void exitChildHandler(int signum){
	shmdt(shm_addr); //detach shared memory segment
	close(logFd); //close log file
	exit(EXIT_SUCCESS);
}


///////////////////////////////////////////////////////////////////////
// printLog							     //
// ================================================================= //
// Input: void* -> log buffer					     //	
//								     //
// Output: void*						     //
//								     //
// Purpose: thread that prints log to log file using semaphore.	     //
///////////////////////////////////////////////////////////////////////
void* printLog(void *log){
	sem_t *logsem = sem_open("LOGSEM", O_RDWR); //get binary semaphore
	sem_wait(logsem); //wait until unlock

	write(logFd, log, strlen((char *)log));
	sem_post(logsem); //unlock
	sem_close(logsem); //close semaphore
}


///////////////////////////////////////////////////////////////////////
// addHistory							     //
// ================================================================= //
// Input: void* -> struct argHistory ponter			     //
//								     //
// Output: void*						     //
//								     //
// Purpose: thread handler that adds connection history		     //
///////////////////////////////////////////////////////////////////////
void* addHistory(void *arg){
	void *shm_addr = ((struct argHistory *)arg)->shm_addr;
	void *table = shm_addr + HISTORY_OFFSET + 2 * sizeof(int); //history table start address
	int historyCnt = 0, orderCnt = 0;

	pthread_mutex_lock(&mutex);
	memcpy(&historyCnt, shm_addr + HISTORY_OFFSET, sizeof(int)); //get history entry count
	memcpy(&orderCnt, shm_addr + HISTORY_OFFSET + sizeof(int), sizeof(int)); //get history order count
	
	int tailOffset = historyCnt - 1, i = 0; //last history entry pointer
	if(historyCnt >= maxHistory){
		tailOffset = maxHistory - 2;
		historyCnt--;
	}

	for(i = tailOffset; i >= 0; i--) //shift items right
		memcpy(table + (i + 1) * sizeof(struct connectInfo), table + i * sizeof(struct connectInfo), sizeof(struct connectInfo));

	struct connectInfo *p = (struct connectInfo *)table; //entry poiter to be filled with new history log
	p->no = ++orderCnt;
	memcpy(p->ipaddr, ((struct argHistory *)arg)->ipaddr, 20);
	p->pid = ((struct argHistory *)arg)->pid;
	p->port = ((struct argHistory *)arg)->port;
	p->connectedTime = ((struct argHistory *)arg)->connectedTime;
	
	historyCnt++; //increase history count
	memcpy(shm_addr + HISTORY_OFFSET, &historyCnt, sizeof(int)); //update history count
	memcpy(shm_addr + HISTORY_OFFSET + sizeof(int), &orderCnt, sizeof(int)); //update order count

	pthread_mutex_unlock(&mutex);
	return NULL;
}


///////////////////////////////////////////////////////////////////////
// printHistory							     //
// ================================================================= //
// Input: void* -> shared memory				     //
//								     //
// Output: void*						     //
//								     //
// Purpose: thread handler that prints histories		     //
///////////////////////////////////////////////////////////////////////
void* printHistory(void *shm_addr){
	void *table = shm_addr + HISTORY_OFFSET + 2 * sizeof(int); //history table start address
	int historyCnt = 0, i;

	pthread_mutex_lock(&mutex);
	memcpy(&historyCnt, shm_addr + HISTORY_OFFSET, sizeof(int)); //get history entry count

	for(i = 0; i < historyCnt; i++){
		struct connectInfo *p = (struct connectInfo *)(table + i * sizeof(struct connectInfo));
		time_t t = p->connectedTime;
		char timebuf[50];

		strftime(timebuf, 50, "%c", gmtime(&t)); //change time_t to formatted string
		printf("%d\t%s\t%d\t%d\t%s\n", p->no, p->ipaddr, p->pid, p->port, timebuf); //print history
	}
	pthread_mutex_unlock(&mutex);
	return NULL;
}


///////////////////////////////////////////////////////////////////////
// printHistoryHandler						     //
// ================================================================= //
// Input: int -> signal number					     //
//								     //
// Output: void							     //
//								     //
// Purpose: SIGALRM handler. calls printHistory()		     //
///////////////////////////////////////////////////////////////////////
void printHistoryHandler(int signum){
	printf("======================= Connection History  =======================\n");
	printf("NO.	IP		PID	PORT	TIME\n");

	pthread_t tid;
	pthread_create(&tid, NULL, printHistory, shm_addr); //call print history thread
	pthread_join(tid, NULL);

	printf("===================================================================\n\n");
	alarm(10); //reset alarm
}


///////////////////////////////////////////////////////////////////////
// chkWhiteList							     //
// ================================================================= //
// Input: char * -> client dotted decimal ip			     //
// Input: char * -> whitelist buffer				     //
//								     //
// Output: char  -> if 1 the client is approved, otherwise 0	     //
//								     //
// Purpose: SIGALRM handler. print access history.		     //
///////////////////////////////////////////////////////////////////////
char chkWhiteList(char *clnt_ip, char *whiteListBuf){
	char *tok = strtok(whiteListBuf, "\n"), emptyFlag = 1; //get whitelist
	while(tok){
		emptyFlag = 0;
		if(!fnmatch(tok, clnt_ip, 0)) //if ip address is matching with whitelist
			return 1; //return 1
		tok = strtok(NULL, "\n"); //check next entry
	}
	return emptyFlag; //if whitelist is empty, emptyFlag is 1, otherwise 0(not allowed)
}


///////////////////////////////////////////////////////////////////////
// sendResponse							     //
// ================================================================= //
// Input: char* -> response header buffer			     //
//	  char* -> response data buffer			             //
//        int   -> response status code			     	     //
//	  char*	-> response status phrase		     	     //
//	  char* -> response content type		     	     //
//	  int	-> response content length		     	     //
//	  int	-> client socket			     	     //
//								     //
// Purpose: make response header and send header and data to client  //
///////////////////////////////////////////////////////////////////////
void sendResponse(char *dataBuf, int code, char *phrase, char *contentType, int contentLength, int clientSocket){
	char headerBuf[256] = {0, };
	int writeSz = 0;

	sprintf(headerBuf, "HTTP/1.1 %d %s\r\nContent-Type:%s\r\nContent-Length:%d\r\n\r\n", code, phrase, contentType, contentLength); //make response header appropriately
	while(writeSz < strlen(headerBuf))
		writeSz += write(clientSocket, headerBuf, strlen(headerBuf)); //send response header
	writeSz = 0;
	while(writeSz < contentLength)
		writeSz += write(clientSocket, dataBuf, contentLength); //send response data
}


///////////////////////////////////////////////////////////////////////
// getNameListDir						     //
// ================================================================= //
// Input: DIR*   -> directory stream pointer to get file name list   //
//		  char** ->	name list pointer		     //
//		  int*   -> list entry count			     //
//		  char	 -> a option flag			     //
//								     //
// Output: long int -> total 1K block count			     //
//								     //
// Purpose: get name list from directory			     //
// Warning: Programmer shoud call chdir() to working directory	     //
//			before call this function.		     //
///////////////////////////////////////////////////////////////////////
long int getNameListDir(DIR *dp, char **nameList,  int *entryCnt, char aflag){
	long int total = 0;
	struct dirent *dent; //direcetory entry struct pointer
	struct stat stFile;
	*entryCnt = 0; //initialize entry count of directory
	
	while((dent = readdir(dp)) != NULL){
		if(aflag || dent->d_name[0] != '.'){ //if aflag is activated or non-hidden file, or correct string with wildcard pattern
			nameList[(*entryCnt)++] = dent->d_name; //add to file name list
			if(lstat(dent->d_name, &stFile) != 0){ //get file attribute
				perror("lstat() from getNameListDir()");
				exit(EXIT_FAILURE);
			}
			total += stFile.st_blocks / 2; //add count of 1K block of file
		}
	}
	return total; //return total 1K blocks
}


///////////////////////////////////////////////////////////////////////
// printFileAttr						     //
// ================================================================= //
// Input: char* -> filename to be printed its file properties	     //
//		  char* -> current working directory		     //
//		  char* -> buffer saving html text to be served.     //
//								     //
// Output: void							     //
//								     //
// Purpose: Print file properties for '-l' option		     //
///////////////////////////////////////////////////////////////////////
void printFileAttr(char *filename, char *workingDir, char *buffer){
	char fMode[11] = {0, }, month[4], temp[512], *tbColor = "red"; //stores file mode and color of string, modified month
	struct stat fInfo;
	struct passwd *owner;
	struct group *grp;
	struct tm *mtime;

	if(lstat(filename, &fInfo) != 0){ //get file attribute
		perror("lstat() from printFileAttr()");
		fputs(filename, stderr);
		fputc('\n', stderr);
		exit(EXIT_FAILURE);
	}

	//file type
	if(S_ISREG(fInfo.st_mode)) fMode[0] = '-'; //regular
	else if(S_ISDIR(fInfo.st_mode)){
		fMode[0] = 'd'; //directory
		tbColor = "blue"; //change color to blue
	}
	else if(S_ISCHR(fInfo.st_mode)) fMode[0] = 'c'; //character special
	else if(S_ISBLK(fInfo.st_mode)) fMode[0] = 'b'; //block special
	else if(S_ISFIFO(fInfo.st_mode)) fMode[0] = 'p'; //named pipe
	else if(S_ISLNK(fInfo.st_mode)){
		fMode[0] = 'l'; //symbolic link
		tbColor = "green"; //chagne color to green
	}
	else if(S_ISSOCK(fInfo.st_mode)) fMode[0] = 's'; //socket
	else fMode[0] = '?'; //unknown file type

	//access permission
	fMode[1] = (fInfo.st_mode & S_IRUSR) ? 'r' : '-'; //user read
	fMode[2] = (fInfo.st_mode & S_IWUSR) ? 'w' : '-'; //user write
	fMode[3] = (fInfo.st_mode & S_IXUSR) ? 'x' : '-'; //user execute
	fMode[3] = (fInfo.st_mode & S_ISUID) ? ((fMode[3] == 'x') ? 's' : 'S') : fMode[3]; //setuid
	fMode[4] = (fInfo.st_mode & S_IRGRP) ? 'r' : '-'; //group read
	fMode[5] = (fInfo.st_mode & S_IWGRP) ? 'w' : '-'; //group write
	fMode[6] = (fInfo.st_mode & S_IXGRP) ? 'x' : '-'; //group execute
	fMode[6] = (fInfo.st_mode & S_ISGID) ? ((fMode[6] == 'x') ? 's' : 'S') : fMode[6]; //setgid
	fMode[7] = (fInfo.st_mode & S_IROTH) ? 'r' : '-'; //other read
	fMode[8] = (fInfo.st_mode & S_IWOTH) ? 'w' : '-'; //other write
	fMode[9] = (fInfo.st_mode & S_IXOTH) ? 'x' : '-'; //other execute
	fMode[9] = (fInfo.st_mode & S_ISVTX) ? ((fMode[9] == 'x') ? 't' : 'T') : fMode[9]; //sticky bit

	//owner info
	if((owner = getpwuid(fInfo.st_uid)) == NULL){ //if getpwuid() error, exit
		perror("getpwuid()");
		exit(EXIT_FAILURE);
	}

	//group info
	if((grp = getgrgid(fInfo.st_gid)) == NULL){ //if getgrgid() error, exit
		perror("getgrgid()");
		exit(EXIT_FAILURE);
	}

	if((mtime = localtime(&fInfo.st_mtime)) == NULL){ //if localtime() error, exit
		perror("localtime()");
		exit(EXIT_FAILURE);
	}
	strftime(month, 4, "%b", mtime); //get month of mtime

	//print file properties
	sprintf(temp, "<tr style=\"color:%s\">\n<td><a href=\"http://%s:%d%s/%s\">%s</a></td>\n<td>%s</td>\n<td>%lu</td>\n<td>%s</td>\n<td>%s</td>\n<td>%ld</td>\n<td>%s %d %02d:%02d</td>\n</tr>\n", tbColor, IP, PORT, workingDir, filename, filename, fMode, fInfo.st_nlink, owner->pw_name, grp->gr_name, fInfo.st_size, month, mtime->tm_mday, mtime->tm_hour, mtime->tm_min);
	strcat(buffer, temp); //accumulate to buffer
}


///////////////////////////////////////////////////////////////////////
// upStr							     //
// ================================================================= //
// Input: char* -> character pointer of string			     //
//								     //
// Purpose: make whole characters of string to uppercase	     //
///////////////////////////////////////////////////////////////////////
char* upStr(char *str){
	char *temp = malloc(strlen(str) + 1); //allocate temporal space
	char *c = temp;
	while(*str){
		*c = toupper(*str); //make character to uppercase
		str++; //move next pointer to change
		c++; //move next pointer  to save
	}
	return temp;
}


///////////////////////////////////////////////////////////////////////
// nameCmp							     //
// ================================================================= //
// Input: char** -> string1 pointer to compare with		     //
//		  char** -> string2 pointer to compare with	     //
//								     //
// Output: char  -> if swapped 1, otherwise 0			     //
//								     //
// Purpose: compare two strings and swap them			     //
///////////////////////////////////////////////////////////////////////
char nameCmp(char **ent1, char **ent2){
	if(!strcmp(*ent1, ".")) return 0; //if str1 is ".", or str2 is "." in r option, don't swap and return false
	if(!strcmp(*ent2, ".")){ //if "." is found but it has to be swapped, swap them
		SWAP(char*, *ent1, *ent2);
		return 1; //return true
	}

	char *n1 = upStr(*ent1), *n2 = upStr(*ent2), swapped = 0; //change to uppercase
	if(strcmp(n1[0] == '.' ? n1 + 1 : n1, n2[0] == '.' ? n2 + 1 : n2) > 0){ //if swap condition is satisfied, swap them. while compare, don't care '.' which is the start character
		SWAP(char*, *ent1, *ent2);
		swapped = 1; //set swap flag
	}
	free(n1); free(n2); //free allocated space
	return swapped; //returns wap flag
}

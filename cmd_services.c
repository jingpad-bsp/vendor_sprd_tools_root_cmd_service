#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <errno.h>
#include <cutils/properties.h>
#include <pthread.h>
#include <utils/Log.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <cutils/sockets.h>

#define MAX_CONNECTION 8     //max connections
#define CMD_BUF_SIZE 256        //command string length
#define BUF_SIZE 4096               //universal buf seize
#define ECHO_SIZE 40960            //command echo size
#define SOCKETS_NAME "cmd_skt"


#define LOGD(format, ...)  ALOGD("%s:%s: "  format "\n" ,"cmd_services", __func__, ## __VA_ARGS__)
#define LOGE(format, ...)  ALOGE("%s:%s: "  format "[%d][%s][%d]\n","cmd_services", __func__,  ## __VA_ARGS__, errno,strerror(errno),__LINE__)

char thread_stat[MAX_CONNECTION] = {0};    //execute command thread
static pthread_mutex_t tid_mtx = PTHREAD_MUTEX_INITIALIZER;
int bgTaskCount=0;
struct cmd_info
{
    char buf[CMD_BUF_SIZE];
    int client_fd;
    int tid_indx;
};

//get free fd
int get_free_fd(int *connected)
{
    int ind = 0;

    for(ind = 0; ind < MAX_CONNECTION; ind++)
    {
        if(connected[ind] == 0)
        {
            return ind;
        }
    }
    return -1;		//no free socket fd
}

char exist_alived_tid()
{
    int ind = 0;
    char thread_stat_temp[MAX_CONNECTION];    //thread status temp

    pthread_mutex_lock(&tid_mtx);       //get thread status, lock another thread
    memcpy(thread_stat_temp, thread_stat, MAX_CONNECTION);
    pthread_mutex_unlock(&tid_mtx);     //unlock another thread

    for(ind = 0; ind < MAX_CONNECTION; ind++)
    {
        if(thread_stat_temp[ind] != 0)		//thread is alived
        {
            return 1;
        }
    }
    if(bgTaskCount>0){
        LOGD("async can't exit,waiting %d",bgTaskCount);
        return 1;
    }
    return 0;//no alived thread
}

int get_free_tid_ind()
{
    int ind = 0;
    char thread_stat_temp[MAX_CONNECTION];    //thread status temp

    pthread_mutex_lock(&tid_mtx);       //get thread status, lock another thread
    memcpy(thread_stat_temp, thread_stat, MAX_CONNECTION);
    pthread_mutex_unlock(&tid_mtx);     //unlock another thread

    for(ind = 0; ind < MAX_CONNECTION; ind++)
    {
        if(thread_stat_temp[ind] == 0)		//thread is not alived
        {
            return ind;
        }
    }
    return -1;		//no free tid, the thread pool is full
}

void *bgTask(void *cmd_para)
{
    char *cmd =(char*)cmd_para;
    char echo_buf[ECHO_SIZE] = {0};
    char buf[BUF_SIZE];
    FILE *file;

    file = popen(cmd, "r");     //open a process by creating a pipe, and invoking the shell

    if(file == NULL)
    {
        LOGD("async [%s], cannot creat Result file!\n",cmd);
    }
    else
    {
        LOGD("async  %s execute success!\n", cmd);
        do
        {
            memset(buf, '\0', sizeof(buf));
            if(fread(buf, sizeof(char), sizeof(buf) - 1, file) <= 0)
            {
                LOGD("async [%s], read command echo_buf error!\n", cmd);
                break;
            }

        }
        while(!feof(file));

        pclose(file);           //close a stream that was opened by popen()
    }


    bgTaskCount--;
    LOGD("async %s exited,bgTaskCount=%d",cmd,bgTaskCount);
    free(cmd);
    return NULL;
}

//execute command in thread
void *exec_cmd(void *cmd_para)
{
    char *cmd = ((struct cmd_info *)cmd_para)->buf;
    int client_fd = ((struct cmd_info *)cmd_para)->client_fd;
    int tid_indx = ((struct cmd_info *)cmd_para)->tid_indx;
    char echo_buf[ECHO_SIZE] = {0};
    char buf[BUF_SIZE];
    FILE *file;

    pthread_detach(pthread_self());
    file = popen(cmd, "r");     //open a process by creating a pipe, and invoking the shell

    if(file == NULL)
    {
        LOGD("tid[%d], cannot creat Result file!\n", tid_indx);
    }
    else
    {
        LOGD("tid[%d], %s execute success!\n", tid_indx, cmd);
        do
        {
            if(strstr(cmd,"asynctask")!=NULL){
                LOGD("tid[%d], asynctask break!\n", tid_indx);
                break;
            }
            memset(buf, '\0', sizeof(buf));
            if(fread(buf, sizeof(char), sizeof(buf) - 1, file) <= 0)
            {
                LOGD("tid[%d], read command echo_buf error!\n", tid_indx);
                break;
            }

            if((strlen(echo_buf) + strlen(buf) + 100) < ECHO_SIZE)
            {
                LOGD("tid[%d], buf: %s\n", tid_indx, buf);
                strcat(echo_buf, buf);      //appends a copy of the ''buf'' to ''echo_buf''
            }
            else            //buf is full
            {
                LOGD("tid[%d], echo_buf buffer is full, buf: %s\n", tid_indx, buf);
                if(write(client_fd, echo_buf, strlen(echo_buf)) <= 0)       //write echo_buf to the client socket
                {
                    LOGD("tid[%d], send part of cmd echo failed!\n",tid_indx);
                }
                memset(echo_buf, '\0', sizeof(echo_buf));       //reset echo_buf
                strcat(echo_buf, buf);      //appends a copy of the ''buf'' to ''echo_buf''
            }
        }
        while(!feof(file));

        pclose(file);           //close a stream that was opened by popen()
    }

    strcat(echo_buf, "Result");
    if(write(client_fd, echo_buf, strlen(echo_buf)) <= 0)       //write echo_buf to the client socket
    {
        LOGD("tid[%d], send cmd echo failed!\n",tid_indx);
    }

    LOGD("tid[%d] exist\n", tid_indx);
    if(tid_indx < MAX_CONNECTION && tid_indx >= 0)
    {
        pthread_mutex_lock(&tid_mtx);		//set thread status, lock another thread
        thread_stat[tid_indx] = 0;
        pthread_mutex_unlock(&tid_mtx);		//unlock another thread
    }
    else
        LOGD("tid is wrong, tid_indx = %d",tid_indx);


    return NULL;
}

//remove the end '\n' from cmd string, and add 2>&1
void cmd_trim(char *cmd)
{
    int len = strlen(cmd);
    while(len-- > 0)
    {
        if(cmd[len] == '\n')
            cmd[len] = '\0';
		else
			break;
    }
    strcat(cmd, " 2>&1");
}

int main(int argc, char *argv[])
{
    int connected_fd[MAX_CONNECTION] = {0};    //accepted connection fd
    pthread_t tid[MAX_CONNECTION];    //execute command thread
    struct cmd_info cmd_temp[MAX_CONNECTION];
    struct sockaddr_un client_address;
    int services_fd = -1, new_client_fd;  //listen on services_fd, new connection on new_client_fd
    int connected_nums = 0, cmd_count = 0;      //total client in connected, total commands on working
    struct timeval timeout;
    socklen_t client_len;
    int free_fd, maxfd;
    fd_set fdsr;
    int indx, i, ret;

/*use for debug, output logs on screen, replace "LOGD" with "printf"*/
//    fflush(stdout);
//    setvbuf(stdout, NULL, _IONBF, 0);

    //creat services socket, bind, listen...
    services_fd = socket_local_server(SOCKETS_NAME, ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM);
    if (services_fd < 0) {
        LOGE("<%s> created fail!", SOCKETS_NAME);
        return -1;
    }

    LOGD("<%s> is created. fd = %d\n", SOCKETS_NAME, services_fd);

    maxfd = services_fd + 1;
    while (1)
    {
        FD_ZERO(&fdsr);        // initialize file descriptor set
        FD_SET(services_fd, &fdsr);
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;

        //add active connection to FD_SET
        for (i = 0; i < MAX_CONNECTION; i++)
        {
            if(connected_fd[i] != 0)
                FD_SET(connected_fd[i], &fdsr);
        }

        ret = select(maxfd, &fdsr, NULL, NULL, &timeout);          //check, connected client data OR new client connection
        if (ret < 0)
        {
            LOGD("select error, continue\n");
            continue;
        }
        else if (ret == 0)
        {
            if(cmd_count > 0 && connected_nums <= 0 && exist_alived_tid() == 0)//client pool and thread pool are empty, exist the socket service
            {
                LOGD("<%s> exist, because 0 clients connected, 0 cmds on working!\n",SOCKETS_NAME);
                break;
            }
            else
            {
                LOGD("%d clients connected, %d cmds on working!\n", connected_nums, cmd_count);
            }
        }
        else if(ret > 0)
        {
            //loop connected client, receive data from client
            for (i = 0; i < MAX_CONNECTION; i++)
            {
                if(connected_fd[i] == 0)
                    continue;
                if(FD_ISSET(connected_fd[i], &fdsr))
                {
                    memset(cmd_temp[i].buf, '\0', sizeof(cmd_temp[i].buf));
                    ret = read(connected_fd[i], cmd_temp[i].buf, sizeof(cmd_temp[i].buf));
                    if(ret > 0)                  //receive data
                    {
                        indx = get_free_tid_ind();
                        cmd_temp[i].tid_indx = indx;
                        cmd_temp[i].client_fd = connected_fd[i];
                        cmd_trim(cmd_temp[i].buf);
                        //strcat(cmd_temp[i].buf, " 2>&1");
                        if(indx >= 0 && indx < MAX_CONNECTION){
                          char* cmd=cmd_temp[i].buf;
                          char* pAsync=strstr(cmd,"ASYNC:");
                          if(pAsync){
                                char *pAsynCmd=(char*)malloc(strlen(cmd)+1);
                                strcpy(pAsynCmd,cmd+strlen("ASYNC:")+1);
                                pthread_t pid;
                                bgTaskCount++;
                                pthread_create(&pid, NULL, bgTask, (void*)pAsynCmd);
                                strcpy(cmd,"echo asynctask");
                                LOGD("async task ,set to %s", cmd);
                          }
                            if(0 == pthread_create(&tid[indx], NULL, exec_cmd, (void*)&cmd_temp[i]))
                            {
                                LOGD("excute client[%d]'s command<%s> in tid[%d]\n", i, cmd_temp[i].buf, indx);
                                pthread_mutex_lock(&tid_mtx);		//set thread status, lock another thread
                                thread_stat[indx] = 1;
                                pthread_mutex_unlock(&tid_mtx);		//unlock another thread
                                cmd_count++;
                            }
                            else
                            {
                                LOGD("cmd thread creat failed!\n");
                            }
                        }
                        else
                        {
                            LOGD("received client[%d]'s command, but cannot excute because thread pool is full\n", i);
                        }
                    }
                    else                           //client close
                    {
                        LOGD("client[%d] close\n", i);
                        close(connected_fd[i]);
                        FD_CLR(connected_fd[i], &fdsr);     //remove connection from FD_SET
                        connected_fd[i] = 0;
                        connected_nums--;
                    }
                }
            }

            free_fd = get_free_fd(connected_fd);

            //check whether a new client connection comes
            if(free_fd >= 0 && FD_ISSET(services_fd, &fdsr))
            {
                new_client_fd = accept(services_fd, (struct sockaddr *)&client_address, &client_len);
                if (new_client_fd >= 0)
                {
                    LOGD("a new client, assign as: client[%d]\n",free_fd);
                    connected_fd[free_fd] = new_client_fd;       //add to fd arrays
                    connected_nums++;
                    if ((new_client_fd + 1) > maxfd)
                        maxfd = new_client_fd + 1;
                }
                else
                {
                    LOGD("fd = %d,a new client, but accept error!\n",new_client_fd);
                    continue;
                }
            }
        }
    }

    return 0;
}

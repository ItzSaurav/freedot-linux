#include <iostream>
#include <unistd.h>
#include <sys/types.h>
int main()
{
    std::cout<<"======================================================\n";
    std::cout<<" FreeDot Minimal C++ Userspace test      \n";
    std::cout<<" Running as PID:"<<getpid()<<", UID:"<<getuid()<<"\n";
    std::cout<<" C++20 Runtime & syscalls functional!  \n";
    std::cout<<"==================================\n";
    return 0;
}
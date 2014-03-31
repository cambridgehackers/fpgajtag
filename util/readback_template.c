#include <stdio.h>
#include <fcntl.h>

unsigned int foo[] = {
 0xFFFFFFFF ,
 0xAA995566 ,
 0x20000000 ,
 0x30008001 ,
 0x00000007 ,
 0x20000000 ,
 0x20000000,
0,
0,
 0xFFFFFFFF ,
 0xAA995566 ,
 0x20000000 ,
 0x30008001 ,
 0x00000004 ,
 0x30002001 ,
 0x00000000 ,
 0x28006000 ,
 0x48024090 ,
 0x20000000 ,
 0x20000000,
};

int main()
{
int i;
unsigned int val;
int fd = creat("xx.1", 0666);

printf("[%s:%d] fd %d\n", __FUNCTION__, __LINE__, fd);
for (i = 0; i < sizeof(foo)/sizeof(foo[0]); i++) {
    val = htonl(foo[i]);
    write(fd, &val, sizeof(val));
}
close(fd);
}

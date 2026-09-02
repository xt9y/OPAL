#ifdef __linux__
#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstring>
#include <iostream>
#include <sstream>

static void emit(int fd,int type,int code,int value){input_event e{};e.type=type;e.code=code;e.value=value;write(fd,&e,sizeof(e));}
int main(){int fd=open("/dev/uinput",O_WRONLY|O_NONBLOCK);if(fd<0){perror("/dev/uinput");return 1;}ioctl(fd,UI_SET_EVBIT,EV_KEY);for(int i=1;i<256;i++)ioctl(fd,UI_SET_KEYBIT,i);ioctl(fd,UI_SET_KEYBIT,BTN_LEFT);ioctl(fd,UI_SET_KEYBIT,BTN_RIGHT);ioctl(fd,UI_SET_KEYBIT,BTN_MIDDLE);ioctl(fd,UI_SET_EVBIT,EV_REL);ioctl(fd,UI_SET_RELBIT,REL_X);ioctl(fd,UI_SET_RELBIT,REL_Y);ioctl(fd,UI_SET_RELBIT,REL_WHEEL);uinput_setup us{};std::strncpy(us.name,"OPAL Remote Input",UINPUT_MAX_NAME_SIZE-1);us.id.bustype=BUS_USB;us.id.vendor=0x4f50;us.id.product=0x414c;if(ioctl(fd,UI_DEV_SETUP,&us)<0||ioctl(fd,UI_DEV_CREATE)<0){perror("uinput setup");return 1;}std::string line;while(std::getline(std::cin,line)){std::istringstream ss(line);std::string t;ss>>t;if(t=="KEY"){int k,d;ss>>k>>d;if(k>0&&k<256)emit(fd,EV_KEY,k,d);}else if(t=="MOUSE"){int x,y;ss>>x>>y;emit(fd,EV_REL,REL_X,x);emit(fd,EV_REL,REL_Y,y);}else if(t=="BUTTON"){int b,d;ss>>b>>d;int code=b==1?BTN_LEFT:b==2?BTN_MIDDLE:BTN_RIGHT;emit(fd,EV_KEY,code,d);}else if(t=="WHEEL"){int v;ss>>v;emit(fd,EV_REL,REL_WHEEL,v);}emit(fd,EV_SYN,SYN_REPORT,0);}ioctl(fd,UI_DEV_DESTROY);close(fd);}
#else
#include <iostream>
int main(){std::cerr<<"opal-input is Linux-only\n";return 1;}
#endif

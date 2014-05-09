/*! @file       src/SerialKDPProxy.c
@abstract   Implements the serial KDP Proxy for remote debugging of xnu
@discussion
Very similar to FireWireKDPProxy from Apple.  KDP Packets (UDP) are listened for on the
KDP port (41139) and encapsulated into UDP over IP over Ethernet which is then escaped
for transmission over the serial port.  Input from stdin (i.e. your typing) is sent
down the serial port as well allowing you to use the serial console.

 Conversely, the code looks for escaped ethernet packets on the serial port side and
 builds up a decoded buffer which will eventually be a full ethernet frame.  Any characters
 received outside of the escape sequence (as determined by the result of kdp_unserialize_packet)
 will be sent to stdout thus allowing you to use the serial console.
*/

/* -- BSD style License --
Copyright (c) 2009, David Elliott
All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright
 notice, this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright
 notice, this list of conditions and the following disclaimer in the
 documentation and/or other materials provided with the distribution.
 * Neither the name of David Elliott nor the
 names of its contributors may be used to endorse or promote products
 derived from this software without specific prior written permission.
 
  THIS SOFTWARE IS PROVIDED BY David Elliott ''AS IS'' AND ANY
  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL David Elliott BE LIABLE FOR ANY
  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


// When building for Linux, define _BSD_SOURCE or else this won't build.
// FIXME: There is an alternate set of structures that is common to both BSD and Linux that I
// probably should've used (and the kernel itself uses these).
//#define _BSD_SOURCE 1

#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/wait.h>

#include "kdp_serial.h"



#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>

// Linux doesn't have sin_len in struct sockaddr_in but OS X does.  Define HAVE_SIN_LEN if you
// want to override.  Otherwise this test works well because sin_len must be set to INET_ADDRSTRLEN
// which is defined on OS X (where it is required) and not on Linux (where you cannot set it).
//#ifndef HAVE_SIN_LEN
//# ifdef INET_ADDRSTRLEN
#  define HAVE_SIN_LEN 0
//# else
//#  define HAVE_SIN_LEN 0
//# endif
//#endif //ndef HAVE_SIN_LEN

/*! @function   stty_serial
@abstract   Runs stty to set the serial port appropriately
@argument   ser         The open file descriptor for the serial port
@discussion
When the last fd to a serial port is closed, the serial port resets to defaults.  Therefore,
stty must be run with the file descriptor already open.
*/
void stty_serial(int ser)
{
    pid_t pid = fork();
    if(pid < 0)
    {
        fprintf(stderr, "Couldn't fork to exec stty\n");
        return;
    }
    else if(pid > 0)
    {
        // parent.. wait for child
        int stat_loc;
        waitpid(pid, &stat_loc, 0);
        return;
    }
    // child
    if(dup2(ser, STDIN_FILENO) != 0)
    {
        fprintf(stderr,"Shit\n");
        _exit(1);
    }
    // FIXME: Should probably close other fds and shit
	
    // Make sure (now duped) stdin; stdout; and stderr stay open across exec
    fcntl(STDIN_FILENO, F_SETFD, 0);
    fcntl(STDOUT_FILENO, F_SETFD, 0);
    fcntl(STDERR_FILENO, F_SETFD, 0);
    execlp("/bin/stty", "/bin/stty", "115200", "raw", "clocal", "crtscts", NULL);
    // NOTREACHED (or at least.. shouldn't be)
    fprintf(stderr, "Failed exec\n");
    _exit(1);
    for(;;)
        ;
}

/*! @function   turn_off_nonblock
@abstract   Clears the O_NONBLOCK flag
@discussion
We have to open the serial port with O_NONBLOCK on OS X because if we don't it
blocks waiting for modem control to become active.  Once we have fixed this
by running stty with the clocal flag we can put the serial port back into blocking
mode so that read/write will block instead of failing.
*/
void turn_off_nonblock(int ser)
{
    int fl = fcntl(ser, F_GETFL);
    if(fl < 0)
    {
        fprintf(stderr, "Failed to get flags on serial fd %d\n", ser);
        return;
    }
    fcntl(ser, F_SETFL, fl & ~O_NONBLOCK);
}

/*! @function   working_poll
@abstract   A poll-like function that works on devices
@discussion
OS X's native poll function cannot handle TTY devices.  So emulate with select.
This isn't needed on Linux although it shouldn't hurt to use it.
*/
int working_poll(struct pollfd fds[], nfds_t nfds, int timeout)
{
    fd_set readfds;
    fd_set writefds;
    fd_set errorfds;
	
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_ZERO(&errorfds);
	
    int i;
    int maxfd = 0;
    for(i = 0; i < nfds; ++i)
    {
        if(fds[i].fd + 1 > maxfd)
            maxfd = fds[i].fd + 1;
		
        if(fds[i].events & POLLIN)
        {   FD_SET(fds[i].fd, &readfds); }
        if(fds[i].events & POLLOUT)
        {   FD_SET(fds[i].fd, &writefds); }
        // TODO: Other types.. we don't need them right now.
    }
	
    int r;
    // poll timeout is millis
    // tv is sec, micros
    struct timeval tv = { timeout / 1000, (timeout % 1000) * 10 };
    
    r = select(maxfd, &readfds, &writefds, &errorfds, timeout!=-1?&tv:NULL);
    if(r <= 0)
        return r;
	
    r = 0;
    for(i = 0; i < nfds; ++i)
    {
        fds[i].revents = 0;
        if(FD_ISSET(fds[i].fd, &readfds))
            fds[i].revents |= POLLIN;
        if(FD_ISSET(fds[i].fd, &writefds))
            fds[i].revents |= POLLOUT;
        if(FD_ISSET(fds[i].fd, &writefds))
            fds[i].revents |= POLLERR;
        if(fds[i].revents != 0)
            ++r;
    }
    return r;
}

// Include APSL code
#include "ip_sum.h"

/*! @variable   ser
@abstract   The FD of the serial port
@discussion
The kdp_serialize_packet function takes a function pointer to write data to
the serial port but does not provide any means to pass context.  So we have
no choice but to make the FD of the serial port a global.
*/
static int ser;

/*! @function   serial_putc
@abstract   Function to put one character to the serial port for kdp_serialize_packet
@discussion
The kdp_serialize_packet function takes a function pointer of this signature and calls
it for each character it wants to output to the serial port.  The ser global variable
is used as the file descriptor to write to.
*/
static void serial_putc(char c)
{
    if(write(ser, &c, 1) != 1)
        fprintf(stderr, "Serial write failed!");
	
#if 0
    if((uint8_t)c == 0xFA)
        fprintf(stderr, "START ");
    else if((uint8_t)c == 0xFB)
        fprintf(stderr, "END\n");
    else
        fprintf(stderr, "%02x ", (uint8_t)c);
#endif
}

// A few structs/unions to make decoding the ethernet frames easier using
// some type punning and the like.
// FIXME: There are more portable structs that can be used.
struct udp_ip_ether_frame_hdr
{
    struct ether_header eh;
    struct ip ih;
    struct udphdr uh;
} __attribute__((packed));

union frame_t 
{
    uint8_t buf[1500];
    struct udp_ip_ether_frame_hdr h;
};
union frame_t frame;

/*!
@abstract   The (fake) MAC address of the kernel's KDP
@discussion
This must be "serial" because the kernel won't allow anything else.
*/
u_char const client_macaddr[ETHER_ADDR_LEN] = { 's', 'e', 'r', 'i', 'a', 'l' };

/*!
@abstract   The (fake) MAC address of our side of the KDP
@discussion
This can be anything really.  But it's more efficient to use characters that
don't need to be escaped by kdp_serialize_packet.
*/
u_char const our_macaddr[ETHER_ADDR_LEN] = { 'f', 'o', 'o', 'b', 'a', 'r' };

/*!
@abstract   The last IP sequence number.
*/
static uint16_t out_ip_id = 0;

/*!
@abstract   A helper function to initialize the new UDP ethernet frame
@argument   pFrame      Pointer to ethernet frame to initialize
@argument   sAddr       Source IP address to use for packet, in network byte order
@argument   sPort       Source UDP port to use for packet, in network byte order
@argument   dataLen     Size of UDP data
*/
void setup_udp_frame(union frame_t *pFrame, struct in_addr sAddr, in_port_t sPort, ssize_t dataLen)
{
    memcpy(pFrame->h.eh.ether_dhost,client_macaddr,ETHER_ADDR_LEN);
    memcpy(pFrame->h.eh.ether_shost,our_macaddr,ETHER_ADDR_LEN);
    pFrame->h.eh.ether_type = htons(ETHERTYPE_IP);
    pFrame->h.ih.ip_v = 4;
    pFrame->h.ih.ip_hl = sizeof(struct ip) >> 2;
    pFrame->h.ih.ip_tos = 0;
    pFrame->h.ih.ip_len = htons(sizeof(struct ip) + sizeof(struct udphdr) + dataLen);
    pFrame->h.ih.ip_id = htons(out_ip_id++);
    pFrame->h.ih.ip_off = 0;
    pFrame->h.ih.ip_ttl = 60; // UDP_TTL from kdp_udp.c
    pFrame->h.ih.ip_p = IPPROTO_UDP;
    pFrame->h.ih.ip_sum = 0;
    pFrame->h.ih.ip_src = sAddr; // Already in NBO
    pFrame->h.ih.ip_dst.s_addr = 0xABADBABE; // FIXME: Endian.. little to little will be fine here.
    //pFrame->h.ih.ip_dst.s_addr = 0xBEBAADAB; // FIXME: Endian.. host big, target little (or vice versa)
    // Ultimately.. the address doesnt seem to actually matter to it.
	
    pFrame->h.ih.ip_sum = htons(~ip_sum((unsigned char *)&pFrame->h.ih, pFrame->h.ih.ip_hl));
	
    pFrame->h.uh.uh_sport = sPort; // Already in NBO
    pFrame->h.uh.uh_dport = htons(41139);
    pFrame->h.uh.uh_ulen = htons(sizeof(struct udphdr) + dataLen);
    pFrame->h.uh.uh_sum = 0; // does it check this shit?
}

// A few definitions so when we output escaped characters we do so in reverse video

#define REVERSE_VIDEO "\x1b[7m"
#define NORMAL_VIDEO "\x1b[0m"


#define SPEED B115200
short InitSerial(int fd)
{
	struct termios tio ;
	
	
	tcgetattr(fd, &tio);
	
	cfsetospeed(&tio, SPEED);
	cfsetispeed(&tio, SPEED);
	tio.c_cflag |= CLOCAL | CREAD;
	
	tio.c_cflag &= ~PARENB;
	tio.c_cflag &= ~CSTOPB;
	tio.c_cflag &= ~CSIZE;
	tio.c_cflag |= CS8;
	
	//tio.c_cflag &= ~CNEW_RTSCTS;
	tio.c_iflag &= ~(IXON | IXOFF | IXANY);
	
	tio.c_cc[VTIME] = 1;
	tio.c_cc[VMIN] = 20;
	
	tio.c_lflag &= ~(ICANON | ECHO | ECHOE);
	tio.c_oflag &= ~OPOST;
	
	tcflush(fd,TCIFLUSH);
	tcsetattr(fd,TCSAFLUSH,&tio);
	
	tcflush(fd, TCOFLUSH);
	return fd;
}

/*!
@abstract   Implementation of SerialKDPProxy
@discussion
Basically there are three input sources here.  The UDP socket for the KDP port,
the serial TTY for characters coming from the kernel, and stdin for characters
coming from the user who wants to type stuff to the kernel's serial console.

 The code assumes that writes (particularly to the serial port) will block on
 flow control but hopefully not for very long.  That is, it has no internal
 buffer of data to be written to the serial port but instead just calls write
 and assumes it will complete soon enough so we don't miss receiving the next
 UDP packet or other data. Try to avoid typing anything into stdin or doing
 anything with GDB while the target machine is in the midst of rebooting because
 CTS will be off and this program will then block until the newly rebooted kernel
 comes up and then starts accepting a flood of data which it probaby didn't need.
*/
int main(int argc, char **argv)
{
    if(argc < 2)
    {
        fprintf(stderr, "%s /dev/ttyXXX\n", argv[0]);
        return 1;
    }
    char const *serialPortTTY = argv[1];
	
    char const *rawSerialDumpFN = argc>=4 ? argv[2] : NULL;
	
    int s = socket(AF_INET,SOCK_DGRAM, 0);
    if(s < 0)
    {
        fprintf(stderr, "Failed to open socket\n");
        return 1;
    }
	
	FILE *fRawSerial = rawSerialDumpFN != NULL?fopen(rawSerialDumpFN, "wb"):NULL;
	
    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
#if HAVE_SIN_LEN
    saddr.sin_len = INET_ADDRSTRLEN;
#endif
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(41139);
    saddr.sin_addr.s_addr = INADDR_ANY;
	
    if(bind(s, (struct sockaddr*)&saddr, sizeof(saddr)) != 0)
    {
        fprintf(stderr, "Failed to bind\n");
        return 1;
    }
	
    fprintf(stderr, "Opening Serial\n");
    // Open serial in non-blocking mode because CD will not be asserted
    // and thus we'd block on the open.  We don't want to block on the
    // open as we want to run stty against it to set the parameters.
    ser = open(serialPortTTY, O_RDWR|O_NONBLOCK);
    if(ser < 0)
    {
        fprintf(stderr, "Failed to open serial\n");
        return 1;
    }
	InitSerial(ser);
    
    // Setup the port to 115200 in raw mode.
	// stty_serial(ser);
    // Turn off non-blocking mode.  We do want to block now.
    turn_off_nonblock(ser);
	
    fprintf(stderr, "Waiting for packets, pid=%lu\n", (long unsigned)getpid());
	
    struct pollfd pollfds[3] =
	{   { s, POLLIN, 0}
	,   { ser, POLLIN, 0}
	,   { STDIN_FILENO, POLLIN, 0}
	};
	
    while(working_poll(pollfds, 3, -1))
    {
        ssize_t bytesReceived = 0;
        if( (pollfds[0].revents & POLLIN) != 0 )
        {
            struct sockaddr clientAddr;
            socklen_t cal = sizeof(clientAddr);
            bytesReceived = recvfrom(s, frame.buf + sizeof(frame.h), sizeof(frame) - sizeof(frame.h), 0, &clientAddr, &cal);
            in_port_t clntPort = ((struct sockaddr_in*)&clientAddr)->sin_port;
			
            fprintf(stderr, "Received %ld bytes from port %d...", bytesReceived, ntohs(clntPort));
            setup_udp_frame(&frame, ((struct sockaddr_in*)&clientAddr)->sin_addr, clntPort, bytesReceived);
            // Output the bytes to the serial port.
            kdp_serialize_packet(frame.buf, bytesReceived + sizeof(frame.h), &serial_putc);
            fprintf(stderr, "and ouput over serial\n");
            fflush(stderr);
        }
        else if( (pollfds[0].revents) != 0)
        {
            fprintf(stderr, "WTF1\n");
        }
		
        if( (pollfds[1].revents & POLLIN) != 0)
        {
            //fprintf(stderr, "Got something on serial\n");
            unsigned char c;
            if(read(ser, &c, 1) == 1)
            {
                // HACK: There is a peculiar case in kdp_unserialize_packet where *len does not
                // get set.  The kernel doesn't hit this bug because it runs kdp_unserialize_packet
                // in a loop where the value of len remains stable across calls because it is
                // declared outside of the loop.  But since we are declaring it well inside the loop
                // we have to account for this case.
                unsigned int len = SERIALIZE_READING;
				
                // For debugging purposes.  This will cause all output to go to the specified file
                // which allows it to be examined for problems.
                if(fRawSerial != NULL)
                    fputc(c, fRawSerial);
				
                union frame_t *pInputFrame = (void*)kdp_unserialize_packet(c, &len);
                if(pInputFrame != NULL)
                {
                    if(pInputFrame->h.ih.ip_p == 17)
                    {
                        // These 3 sizes should theoretically match.
                        size_t udpDataLen = ntohs(pInputFrame->h.uh.uh_ulen) - sizeof(struct udphdr);
                        size_t ipDataLen = ntohs(pInputFrame->h.ih.ip_len) - sizeof(struct ip) - sizeof(struct udphdr);
                        size_t frameDataLen = len - sizeof(pInputFrame->h);
						
                        // send packet
                        struct sockaddr_in clientAddr;
                        bzero(&clientAddr, sizeof(clientAddr));
#if HAVE_SIN_LEN
                        clientAddr.sin_len = INET_ADDRSTRLEN;
#endif
                        clientAddr.sin_family = AF_INET;
                        clientAddr.sin_port = pInputFrame->h.uh.uh_dport;
						clientAddr.sin_addr = pInputFrame->h.ih.ip_dst;
						//clientAddr.sin_addr.s_addr = inet_aton("192.168.17.204");
                        //bzero(clientAddr.sin_zero, sizeof(clientAddr.sin_zero));
                        sendto(s, pInputFrame->buf + sizeof(pInputFrame->h), frameDataLen, 0, (struct sockaddr*)&clientAddr, sizeof(clientAddr));
						fflush(stdout);
                        fprintf(stderr, "Sent reply packet %d, %d, %d to UDP %d  \n", (int)udpDataLen, (int)ipDataLen, (int)frameDataLen, (int)ntohs(pInputFrame->h.uh.uh_dport));
						fflush(stderr);
                    }
                    else
                    {
                        fprintf(stderr, "Discarding non-UDP packet proto %d of length %u\n", pInputFrame->h.ih.ip_p, len);
                    }
                    // Flush the raw serial debug output any time we get a packet.
                    if(fRawSerial != NULL)
                        fflush(fRawSerial);
                }
                else
                {
                    if(len == SERIALIZE_WAIT_START)
                    {
                        uint8_t b = c;
                        if( (b >= 0x80) || (b > 26 && b < ' ') )
                        {
                            printf(REVERSE_VIDEO "\\x%02x" NORMAL_VIDEO, b);
                            fflush(stdout);
                        }
                        else if( (b <= 26) && (b != '\r') && (b != '\n') )
                        {
                            printf(REVERSE_VIDEO "^%c" NORMAL_VIDEO, b + '@'); // 0 = @, 1 = A, ..., 26 = Z
                            fflush(stdout);
                        }
                        else
                            putchar(c); // Write character from serial to console
                    }
                }
            }
        }
        else if( (pollfds[1].revents) != 0)
        {
            fprintf(stderr, "Shutting down serial input due to 0x%x\n",pollfds[1].revents);
            pollfds[1].events = 0;
        }
		
        if( (pollfds[2].revents & POLLIN) != 0)
        {
            //fprintf(stderr, "Got something from console\n");
            uint8_t consoleBuf[1];
            bytesReceived = read(pollfds[2].fd, consoleBuf, 1);
            // Write console input to the serial port
            write(ser, consoleBuf, bytesReceived);
        }
        else if( (pollfds[2].revents) != 0)
        {
            fprintf(stderr, "Shutting down console input due to 0x%x\n", pollfds[2].revents);
            pollfds[2].events = 0;
        }
    }
    return 0;
}


/*
 * bsd_epoll.h - Linux epoll.h compatible header
 *
 * Signed-off-by: Boaz Harrosh <bharrosh@panasas.com> 2009
 *
 * description:
 *    This file makes any code that was compiled with <sys/epoll.h> header
 *    on Linux compile-able under FreeBSD and derivatives. It is meant to be
 *    used Together with bsd_epoll.c source file, to also be run-able.
 *
 * Example useage:
 *   Any code that was using epoll.h before can now do:
 *        #if defined(__linux__)
 *        #        include <sys/epoll.h>
 *        #else
 *        #        include "bsd_epoll.h"
 *        #endif
 *  Under FreeBSD the Makefile should add the bsd_epoll.c source file
 *
 * License:
 *      This header file is not under any license it has been long claimed
 *      and legally proven that Interfaces cannot be copyrighted.
 *      Now since this file only defines interfaces originally put forth
 *      by the Linux Kernel, they are not governed by any License.
 *      In anyway, Linus Trovalds, the originator of the Linux Kernel as
 *      Stated that the Linux Kernel's user-mode APIs are not copyrighted.
 *
 *      This file was hand crafted by me based on Linux Kernel sources
 *      and GlibC's header epoll.h to be made code compatible with epoll.h
 *      Header on Linux.
 *
 *      However, the implementation file bsd_epoll.c is copyrighted under
 *      the "New BSD License" so it can be included in the tirpc library
 *      project.
 *      But I fully expect that if you make any fixes/enhancements to
 *      bsd_epoll.c you shall send these changes to me for inclusion
 *      in the next version. (Or I'll hunt your dreams, and you will
 *      not have peace)
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef __BSD_EPOLL_H__
#define __BSD_EPOLL_H__

#include <stdint.h>

union epoll_data {
	int fd;
	uint32_t u32;
	uint64_t u64;
	void *ptr;
};

struct epoll_event {
	uint32_t events;
	union epoll_data data;
};

enum EPOLL_EVENTS {
	EPOLLIN = 0x001,
#define EPOLLIN EPOLLIN
	EPOLLPRI = 0x002,
#define EPOLLPRI EPOLLPRI
	EPOLLOUT = 0x004,
#define EPOLLOUT EPOLLOUT
/*
    EPOLLRDNORM = 0x040,
#define EPOLLRDNORM EPOLLRDNORM
    EPOLLRDBAND = 0x080,
#define EPOLLRDBAND EPOLLRDBAND
    EPOLLWRNORM = 0x100,
#define EPOLLWRNORM EPOLLWRNORM
    EPOLLWRBAND = 0x200,
#define EPOLLWRBAND EPOLLWRBAND
    EPOLLMSG = 0x400,
#define EPOLLMSG EPOLLMSG
    EPOLLERR = 0x008,
#define EPOLLERR EPOLLERR
    EPOLLHUP = 0x010,
#define EPOLLHUP EPOLLHUP
*/
	EPOLLRDHUP = 0x2000,
#define EPOLLRDHUP EPOLLRDHUP
	EPOLLONESHOT = (1 << 30),
#define EPOLLONESHOT EPOLLONESHOT
	EPOLLET = (1 << 31)
#define EPOLLET EPOLLET
};

#define EPOLL_CTL_ADD     1
#define EPOLL_CTL_DEL     2
#define EPOLL_CTL_MOD     3

#define EPOLL_MAX_EVENTS        (INT_MAX / sizeof(struct epoll_event))

extern int epoll_create(int size /*unused */);
extern int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
extern int epoll_wait(int epfd, struct epoll_event *events, int maxevents,
		      int timeout);

#endif /* __BSD_EPOLL_H__ */

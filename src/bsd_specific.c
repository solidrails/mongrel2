/**
 *
 * Copyright (c) 2010, Zed A. Shaw and Mongrel2 Project Contributors.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 * 
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 * 
 *     * Neither the name of the Mongrel2 Project, Zed A. Shaw, nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <task/task.h>
#include <dbg.h>


#if defined(__APPLE__) || defined(__FreeBSD__)

/**
 * BSD version of sendfile, which is OSX and FreeBSD mostly.
 */
int bsd_sendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
    off_t my_count = count;
    int rc;

    // We have to do this loop nastiness, because mac os x fails with resource
    // temporarily unavailable (per bug e8eddb51a8)
    do {
        fdwait(out_fd, 'w');
#if defined(__APPLE__)
        rc = sendfile(in_fd, out_fd, *offset, &my_count, NULL, 0);
#elif defined(__FreeBSD__)
        rc = sendfile(in_fd, out_fd, *offset, count, NULL, &my_count, 0);
#endif
        *offset += my_count;
    } while(rc != 0 && errno == 35);

    check(rc == 0, "OS X sendfile wrapper failed");

    return my_count;

error:
    return -1;
}

#else

extern int fdrecv(int fd, void *buf, int n);
extern int fdsend(int fd, void *buf, int n);

#define BSD_SENDFILE_BUF_SIZE 16384
#include <unistd.h>

/** For the BSDs without sendfile like open and net.**/

int bsd_sendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
   char buf[BSD_SENDFILE_BUF_SIZE];
   int tot, cur, rem, sent;
   int ret = -1;
   off_t orig_offset = 0;

   if (offset != NULL) {
     orig_offset = lseek(in_fd, 0, SEEK_CUR);
     check(orig_offset >= 0, "lseek failure when determining current position");
     check(lseek(in_fd, *offset, SEEK_SET) >= 0, "lseek failure when setting new position");
   }

   for (tot = 0, rem = count, cur = rem; cur != 0 && tot < count; tot += cur, rem -= cur) {
     if (rem >= BSD_SENDFILE_BUF_SIZE) {
       cur = BSD_SENDFILE_BUF_SIZE;
     } else {
       cur = rem;
     }

     cur = fdread(in_fd, buf, cur);
     check(cur >= 0, "Internal sendfile emulation failed: fdread: %i", cur);

     if (cur != 0) {
       sent = fdwrite(out_fd, buf, cur); 
       check(sent == cur, "Internal sendfile emulation failed: fdread: %i, fdwrite: %i", cur, sent);
     }
   }
   
   ret = tot;

error:
   if (offset != NULL) {
     if (ret != -1) {
       *offset += tot;
     }
     lseek(in_fd, orig_offset, SEEK_SET);
   }
   return ret;
}

#endif

/*	$Id$	*/

/*
 * Copyright (c) 1995 Gordon Ross, Adam Glass
 * Copyright (c) 1992 Regents of the University of California.
 * All rights reserved.
 *
 * This software was developed by the Computer Systems Engineering group
 * at Lawrence Berkeley Laboratory under DARPA contract BG 91-66 and
 * contributed to Berkeley.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Lawrence Berkeley Laboratory and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * based on:
 *      nfs/krpc_subr.c
 *	$NetBSD: krpc_subr.c,v 1.10 1995/08/08 20:43:43 gwr Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/conf.h>
#include <sys/sockio.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/mbuf.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/socketvar.h>

#include <net/if.h>
#include <net/route.h>

#include <netinet/in.h>
#include <net/if_types.h>
#include <net/if_dl.h>
#include <netinet/if_ether.h>

#include <nfs/rpcv2.h>
#include <nfs/nfsproto.h>
#include <nfs/nfs.h>
#include <nfs/nfsdiskless.h>
#include <nfs/krpc.h>
#include <nfs/xdr_subs.h>

#define BOOTP_MIN_LEN		300	/* Minimum size of bootp udp packet */

/*
 * What is the longest we will wait before re-sending a request?
 * Note this is also the frequency of "RPC timeout" messages.
 * The re-send loop count sup linearly to this maximum, so the
 * first complaint will happen after (1+2+3+4+5)=15 seconds.
 */
#define	MAX_RESEND_DELAY 5	/* seconds */

/* Definitions from RFC951 */
struct bootp_packet {
  u_int8_t op;
  u_int8_t htype;
  u_int8_t hlen;
  u_int8_t hops;
  u_int32_t xid;
  u_int16_t secs;
  u_int16_t flags;
  struct in_addr ciaddr;
  struct in_addr yiaddr;
  struct in_addr siaddr;
  struct in_addr giaddr;
  unsigned char chaddr[16];
  char sname[64];
  char file[128];
  unsigned char vend[256];
};

#define IPPORT_BOOTPC 68
#define IPPORT_BOOTPS 67

extern int nfs_diskless_valid;
extern struct nfsv3_diskless nfsv3_diskless;

/* mountd RPC */
#if !defined(__rtems__)
static int md_mount __P((struct sockaddr_in *mdsin, char *path,
	u_char *fhp, int *fhsizep, struct nfs_args *args,struct proc *procp));
static int md_lookup_swap __P((struct sockaddr_in *mdsin,char *path,
			       u_char *fhp, int *fhsizep, 
			       struct nfs_args *args,
			       struct proc *procp));
static int setfs __P((struct sockaddr_in *addr, char *path, char *p));
static int getdec __P((char **ptr));
#endif
static char *substr __P((char *a,char *b));
#if !defined(__rtems__)
static void mountopts __P((struct nfs_args *args, char *p)); 
static int xdr_opaque_decode __P((struct mbuf **ptr,u_char *buf,
				  int len));
static int xdr_int_decode __P((struct mbuf **ptr,int *iptr));
#endif
static void printip __P((char *prefix,struct in_addr addr));

#ifdef BOOTP_DEBUG
void bootpboot_p_sa(struct sockaddr *sa,struct sockaddr *ma);
void bootpboot_p_ma(struct sockaddr *ma);
void bootpboot_p_rtentry(struct rtentry *rt);
void bootpboot_p_tree(struct radix_node *rn);
void bootpboot_p_rtlist(void);
void bootpboot_p_iflist(void);
#endif

int  bootpc_call(struct bootp_packet *call,
		 struct bootp_packet *reply,
		 struct proc *procp);

int bootpc_fakeup_interface(struct ifreq *ireq,struct socket *so,
			struct proc *procp);

int 
bootpc_adjust_interface(struct ifreq *ireq,struct socket *so,
			struct sockaddr_in *myaddr,
			struct sockaddr_in *netmask,
			struct sockaddr_in *gw,
			struct proc *procp);

void bootpc_init(void);

#ifdef BOOTP_DEBUG
void bootpboot_p_sa(sa,ma)
     struct sockaddr *sa;
     struct sockaddr *ma;
{
  if (!sa) {
    printf("(sockaddr *) <null>");
    return;
  }
  switch (sa->sa_family) {
  case AF_INET:
    {
      struct sockaddr_in *sin = (struct sockaddr_in *) sa;
      printf("inet %x",ntohl(sin->sin_addr.s_addr));
      if (ma) {
	struct sockaddr_in *sin = (struct sockaddr_in *) ma;
	printf(" mask %x",ntohl(sin->sin_addr.s_addr));
      }
    }
  break;
  case AF_LINK:
    {
      struct sockaddr_dl *sli = (struct sockaddr_dl *) sa;
      int i;
      printf("link %.*s ",sli->sdl_nlen,sli->sdl_data);
      for (i=0;i<sli->sdl_alen;i++) {
	if (i>0)
	  printf(":");
	printf("%x",(unsigned char) sli->sdl_data[i+sli->sdl_nlen]);
      }
    }
  break;
  default:
    printf("af%d",sa->sa_family);
  }
}

void bootpboot_p_ma(ma)
     struct sockaddr *ma;
{
  if (!ma) {
    printf("<null>");
    return;
  }
  printf("%x",*(int*)ma);
}

void bootpboot_p_rtentry(rt)
     struct rtentry *rt;
{
  bootpboot_p_sa(rt_key(rt),rt_mask(rt));
  printf(" ");
  bootpboot_p_ma(rt->rt_genmask);
  printf(" ");
  bootpboot_p_sa(rt->rt_gateway,NULL);
  printf(" ");
  printf("flags %x",(unsigned short) rt->rt_flags);
  printf(" %d",rt->rt_rmx.rmx_expire);
  printf(" %s%d\n",rt->rt_ifp->if_name,rt->rt_ifp->if_unit);
}
void  bootpboot_p_tree(rn)
     struct radix_node *rn;
{
  while (rn) {
    if (rn->rn_b < 0) {
      if (rn->rn_flags & RNF_ROOT) {
      } else {
	bootpboot_p_rtentry((struct rtentry *) rn);
      }
      rn = rn->rn_dupedkey;
    } else {
      bootpboot_p_tree(rn->rn_l);
      bootpboot_p_tree(rn->rn_r);
      return;
    }
    
  }
}

void bootpboot_p_rtlist(void)
{
  printf("Routing table:\n");
  bootpboot_p_tree(rt_tables[AF_INET]->rnh_treetop);
}

void bootpboot_p_iflist(void)
{
  struct ifnet *ifp;
  struct ifaddr *ifa;
  printf("Interface list:\n");
  for (ifp = TAILQ_FIRST(&ifnet); ifp != 0; ifp = TAILQ_NEXT(ifp,if_link))
    {
      for (ifa = TAILQ_FIRST(&ifp->if_addrhead) ;ifa; 
	   ifa=TAILQ_NEXT(ifa,ifa_link))
	if (ifa->ifa_addr->sa_family == AF_INET ) {
	  printf("%s%d flags %x, addr %x, bcast %x, net %x\n",
		 ifp->if_name,ifp->if_unit,
		 (unsigned short) ifp->if_flags,
		 ntohl(((struct sockaddr_in *) ifa->ifa_addr)->sin_addr.s_addr),
		 ntohl(((struct sockaddr_in *) ifa->ifa_dstaddr)->sin_addr.s_addr),
		 ntohl(((struct sockaddr_in *) ifa->ifa_netmask)->sin_addr.s_addr)
		 );
	}
    }
}
#endif

int
bootpc_call(call,reply,procp)
     struct bootp_packet *call;
     struct bootp_packet *reply;	/* output */
     struct proc *procp;
{
	struct socket *so;
	struct sockaddr_in *sin;
	struct mbuf *m, *nam;
	struct uio auio;
	struct iovec aio;
	int error, rcvflg, timo, secs, len;

	/* Free at end if not null. */
	nam = NULL;

	/*
	 * Create socket and set its recieve timeout.
	 */
	if ((error = socreate(AF_INET, &so, SOCK_DGRAM, 0,procp)))
		goto out;

	m = m_get(M_WAIT, MT_SOOPTS);
	if (m == NULL) {
		error = ENOBUFS;
		goto out;
	} else {
		struct timeval *tv;
		tv = mtod(m, struct timeval *);
		m->m_len = sizeof(*tv);
		tv->tv_sec = 1;
		tv->tv_usec = 0;
		if ((error = sosetopt(so, SOL_SOCKET, SO_RCVTIMEO, m)))
			goto out;
	}

	/*
	 * Enable broadcast.
	 */
	{
		int *on;
		m = m_get(M_WAIT, MT_SOOPTS);
		if (m == NULL) {
			error = ENOBUFS;
			goto out;
		}
		on = mtod(m, int *);
		m->m_len = sizeof(*on);
		*on = 1;
		if ((error = sosetopt(so, SOL_SOCKET, SO_BROADCAST, m)))
			goto out;
	}

	/*
	 * Bind the local endpoint to a bootp client port.
	 */
	m = m_getclr(M_WAIT, MT_SONAME);
	sin = mtod(m, struct sockaddr_in *);
	sin->sin_len = m->m_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = INADDR_ANY;
	sin->sin_port = htons(IPPORT_BOOTPC);
	error = sobind(so, m);
	m_freem(m);
	if (error) {
		printf("bind failed\n");
		goto out;
	}

	/*
	 * Setup socket address for the server.
	 */
	nam = m_get(M_WAIT, MT_SONAME);
	if (nam == NULL) {
		error = ENOBUFS;
		goto out;
	}
	sin = mtod(nam, struct sockaddr_in *);
	sin-> sin_len = sizeof(*sin);
	sin-> sin_family = AF_INET;
	sin->sin_addr.s_addr = INADDR_BROADCAST;
	sin->sin_port = htons(IPPORT_BOOTPS);

	nam->m_len = sizeof(*sin);

	/*
	 * Send it, repeatedly, until a reply is received,
	 * but delay each re-send by an increasing amount.
	 * If the delay hits the maximum, start complaining.
	 */
	timo = 0;
	for (;;) {
		/* Send BOOTP request (or re-send). */
		
		aio.iov_base = (caddr_t) call;
		aio.iov_len = sizeof(*call);
		
		auio.uio_iov = &aio;
		auio.uio_iovcnt = 1;
		auio.uio_segflg = UIO_SYSSPACE;
		auio.uio_rw = UIO_WRITE;
		auio.uio_offset = 0;
		auio.uio_resid = sizeof(*call);
		auio.uio_procp = procp;

		error = sosend(so, nam, &auio, NULL, NULL, 0);
		if (error) {
			printf("bootpc_call: sosend: %d\n", error);
			goto out;
		}

		/* Determine new timeout. */
		if (timo < MAX_RESEND_DELAY)
			timo++;
		else
			printf("BOOTP timeout for server 0x%x\n",
			       (int)ntohl(sin->sin_addr.s_addr));

		/*
		 * Wait for up to timo seconds for a reply.
		 * The socket receive timeout was set to 1 second.
		 */
		secs = timo;
		while (secs > 0) {
			aio.iov_base = (caddr_t) reply;
			aio.iov_len = sizeof(*reply);

			auio.uio_iov = &aio;
			auio.uio_iovcnt = 1;
			auio.uio_segflg = UIO_SYSSPACE;
			auio.uio_rw = UIO_READ;
			auio.uio_offset = 0;
			auio.uio_resid = sizeof(*reply);
			auio.uio_procp = procp;
			
			rcvflg = 0;
			error = soreceive(so, NULL, &auio, NULL, NULL, &rcvflg);
			if (error == EWOULDBLOCK) {
				secs--;
				call->secs=htons(ntohs(call->secs)+1);
				continue;
			}
			if (error)
				goto out;
			len = sizeof(*reply) - auio.uio_resid;

			/* Do we have the required number of bytes ? */
			if (len < BOOTP_MIN_LEN)
				continue;

			/* Is it the right reply? */
			if (reply->op != 2)
			  continue;

			if (reply->xid != call->xid)
				continue;

			if (reply->hlen != call->hlen)
			  continue;

			if (bcmp(reply->chaddr,call->chaddr,call->hlen))
			  continue;

			goto gotreply;	/* break two levels */

		} /* while secs */
	} /* forever send/receive */

	error = ETIMEDOUT;
	goto out;

 gotreply:
 out:
	if (nam) m_freem(nam);
	soclose(so);
	return error;
}

int 
bootpc_fakeup_interface(struct ifreq *ireq,struct socket *so,
			struct proc *procp)
{
  struct sockaddr_in *sin;
  int error;
  struct sockaddr_in dst;
  struct sockaddr_in gw;
  struct sockaddr_in mask;

  /*
   * Bring up the interface.
   *
   * Get the old interface flags and or IFF_UP into them; if
   * IFF_UP set blindly, interface selection can be clobbered.
   */
  error = ifioctl(so, SIOCGIFFLAGS, (caddr_t)ireq, procp);
  if (error)
    panic("bootpc_fakeup_interface: GIFFLAGS, error=%d", error);
  ireq->ifr_flags |= IFF_UP;
  error = ifioctl(so, SIOCSIFFLAGS, (caddr_t)ireq, procp);
  if (error)
    panic("bootpc_fakeup_interface: SIFFLAGS, error=%d", error);

  /*
   * Do enough of ifconfig(8) so that the chosen interface
   * can talk to the servers.  (just set the address)
   */
  
  /* addr is 0.0.0.0 */
  
  sin = (struct sockaddr_in *)&ireq->ifr_addr;
  bzero((caddr_t)sin, sizeof(*sin));
  sin->sin_len = sizeof(*sin);
  sin->sin_family = AF_INET;
  sin->sin_addr.s_addr = INADDR_ANY;
  error = ifioctl(so, SIOCSIFADDR, (caddr_t)ireq, procp);
  if (error)
    panic("bootpc_fakeup_interface: set if addr, error=%d", error);
  
  /* netmask is 0.0.0.0 */
  
  sin = (struct sockaddr_in *)&ireq->ifr_addr;
  bzero((caddr_t)sin, sizeof(*sin));
  sin->sin_len = sizeof(*sin);
  sin->sin_family = AF_INET;
  sin->sin_addr.s_addr = INADDR_ANY;
  error = ifioctl(so, SIOCSIFNETMASK, (caddr_t)ireq, procp);
  if (error)
    panic("bootpc_fakeup_interface: set if net addr, error=%d", error);
  
  /* Broadcast is 255.255.255.255 */
  
  sin = (struct sockaddr_in *)&ireq->ifr_addr;
  bzero((caddr_t)sin, sizeof(*sin));
  sin->sin_len = sizeof(*sin);
  sin->sin_family = AF_INET;
  sin->sin_addr.s_addr = INADDR_BROADCAST;
  error = ifioctl(so, SIOCSIFBRDADDR, (caddr_t)ireq, procp);
  if (error)
    panic("bootpc_fakeup_interface: set if broadcast addr, error=%d", error);
  
  /* Add default route to 0.0.0.0 so we can send data */
  
  bzero((caddr_t) &dst, sizeof(dst));
  dst.sin_len=sizeof(dst);
  dst.sin_family=AF_INET;
  dst.sin_addr.s_addr = htonl(0);
  
  bzero((caddr_t) &gw, sizeof(gw));
  gw.sin_len=sizeof(gw);
  gw.sin_family=AF_INET;
  gw.sin_addr.s_addr = htonl(0x0);
  
  bzero((caddr_t) &mask, sizeof(mask));
  mask.sin_len=sizeof(mask);
  mask.sin_family=AF_INET;
  mask.sin_addr.s_addr = htonl(0);
  
  error = rtrequest(RTM_ADD, 
		    (struct sockaddr *) &dst, 
		    (struct sockaddr *) &gw,
		    (struct sockaddr *) &mask, 
		    RTF_UP | RTF_STATIC
		    , NULL);
  if (error)
    printf("bootpc_fakeup_interface: add default route, error=%d\n", error);
  return error;
}

int 
bootpc_adjust_interface(struct ifreq *ireq,struct socket *so,
			struct sockaddr_in *myaddr,
			struct sockaddr_in *netmask,
			struct sockaddr_in *gw,
			struct proc *procp)
{
  int error;
  struct sockaddr_in oldgw;
  struct sockaddr_in olddst;
  struct sockaddr_in oldmask;
  struct sockaddr_in *sin;

  /* Remove old default route to 0.0.0.0 */
  
  bzero((caddr_t) &olddst, sizeof(olddst));
  olddst.sin_len=sizeof(olddst);
  olddst.sin_family=AF_INET;
  olddst.sin_addr.s_addr = INADDR_ANY;
  
  bzero((caddr_t) &oldgw, sizeof(oldgw));
  oldgw.sin_len=sizeof(oldgw);
  oldgw.sin_family=AF_INET;
  oldgw.sin_addr.s_addr = INADDR_ANY;
  
  bzero((caddr_t) &oldmask, sizeof(oldmask));
  oldmask.sin_len=sizeof(oldmask);
  oldmask.sin_family=AF_INET;
  oldmask.sin_addr.s_addr = INADDR_ANY;
  
  error = rtrequest(RTM_DELETE, 
		    (struct sockaddr *) &olddst,
		    (struct sockaddr *) &oldgw,
		    (struct sockaddr *) &oldmask, 
		    (RTF_UP | RTF_STATIC), NULL);
  if (error) {
    printf("nfs_boot: del default route, error=%d\n", error);
    return error;
  }

  /*
   * Do enough of ifconfig(8) so that the chosen interface
   * can talk to the servers.  (just set the address)
   */
  bcopy(netmask,&ireq->ifr_addr,sizeof(*netmask));
  error = ifioctl(so, SIOCSIFNETMASK, (caddr_t)ireq, procp);
  if (error)
    panic("nfs_boot: set if netmask, error=%d", error);

  /* Broadcast is with host part of IP address all 1's */
  
  sin = (struct sockaddr_in *)&ireq->ifr_addr;
  bzero((caddr_t)sin, sizeof(*sin));
  sin->sin_len = sizeof(*sin);
  sin->sin_family = AF_INET;
  sin->sin_addr.s_addr = myaddr->sin_addr.s_addr | ~ netmask->sin_addr.s_addr;
  error = ifioctl(so, SIOCSIFBRDADDR, (caddr_t)ireq, procp);
  if (error)
    panic("bootpc_call: set if broadcast addr, error=%d", error);
  
  bcopy(myaddr,&ireq->ifr_addr,sizeof(*myaddr));
  error = ifioctl(so, SIOCSIFADDR, (caddr_t)ireq, procp);
  if (error)
    panic("nfs_boot: set if addr, error=%d", error);

  /* Add new default route */

  error = rtrequest(RTM_ADD, 
		    (struct sockaddr *) &olddst,
		    (struct sockaddr *) gw,
		    (struct sockaddr *) &oldmask,
		    (RTF_UP | RTF_GATEWAY | RTF_STATIC), NULL);
  if (error) {
    printf("nfs_boot: add net route, error=%d\n", error);
    return error;
  }

  return 0;
}

#if !defined(__rtems__)
static int setfs(addr, path, p)
	struct sockaddr_in *addr;
	char *path;
	char *p;
{
	unsigned ip = 0;
	int val;

	if (((val = getdec(&p)) < 0) || (val > 255)) return(0);
	ip = val << 24;
	if (*p != '.') return(0);
	p++;
	if (((val = getdec(&p)) < 0) || (val > 255)) return(0);
	ip |= (val << 16);
	if (*p != '.') return(0);
	p++;
	if (((val = getdec(&p)) < 0) || (val > 255)) return(0);
	ip |= (val << 8);
	if (*p != '.') return(0);
	p++;
	if (((val = getdec(&p)) < 0) || (val > 255)) return(0);
	ip |= val;
	if (*p != ':') return(0);
	p++;

	addr->sin_addr.s_addr = htonl(ip);
	addr->sin_len = sizeof(struct sockaddr_in);
	addr->sin_family = AF_INET;

	strncpy(path,p,MNAMELEN-1);
	return(1);
}
#endif

static int getdec(ptr)
	char **ptr;
{
	char *p = *ptr;
	int ret=0;
	if ((*p < '0') || (*p > '9')) return(-1);
	while ((*p >= '0') && (*p <= '9')) {
		ret = ret*10 + (*p - '0');
		p++;
	}
	*ptr = p;
	return(ret);
}

static char *substr(a,b)
	char *a,*b;
{
	char *loc1;
	char *loc2;

        while (*a != '\0') {
                loc1 = a;
                loc2 = b;
                while (*loc1 == *loc2++) {
                        if (*loc1 == '\0') return (0);
                        loc1++;
                        if (*loc2 == '\0') return (loc1);
                }
        a++;
        }
        return (0);
}

static void printip(char *prefix,struct in_addr addr)
{
  unsigned int ip;

  ip = ntohl(addr.s_addr);

  printf("%s is %d.%d.%d.%d\n",prefix,
	 ip >> 24, (ip >> 16) & 255 ,(ip >> 8) & 255 ,ip & 255 );
}

static int dhcpOptionOverload = 0;
static char dhcp_gotgw = 0;
static char dhcp_gotnetmask = 0;
static char dhcp_gotserver = 0;
static char dhcp_gotlogserver = 0;
static struct sockaddr_in dhcp_netmask;
static struct sockaddr_in dhcp_gw;

static void
processOptions (unsigned char *optbuf, int optbufSize)
{
  int j = 0;
  int len;
  int code, ncode;
  char *p;

  ncode = optbuf[0];
  while (j < optbufSize) {
    code = optbuf[j] = ncode;
    if (code == 255)
      return;
    if (code == 0) {
      j++;
      continue;
    }
    len = optbuf[j+1];
    j += 2;
    if ((len + j) >= optbufSize) {
      printf ("Truncated field for code %d", code);
      return;
    }
    ncode = optbuf[j+len];
    optbuf[j+len] = '\0';
    p = &optbuf[j];
    j += len;

    /*
     * Process the option
     */
    switch (code) {
    case 1:
      /* Subnet mask */
      if (len!=4) 
        panic("bootpc: subnet mask len is %d",len);
      bcopy (p, &dhcp_netmask.sin_addr, 4);
      dhcp_gotnetmask = 1;
      break;

    case 2:    /* Time offset, unused */
      break;

    case 3:
      /* Routers */
      if (len % 4) 
        panic ("bootpc: Router Len is %d", len);
      if (len > 0) {
        bcopy(p, &dhcp_gw.sin_addr, 4);
	dhcp_gotgw = 1;
      }
      break;

    case 6:
      /* Domain Name servers */
      if (len % 4) 
        panic ("bootpc: DNS Len is %d", len);
      {
      int dlen = 0;
      while ((dlen < len) &&
             (rtems_bsdnet_nameserver_count < sizeof rtems_bsdnet_config.name_server /
        sizeof rtems_bsdnet_config.name_server[0])) {
        bcopy (p+dlen,
        &rtems_bsdnet_nameserver[rtems_bsdnet_nameserver_count],
        4);
        printip("Domain Name Server",
          rtems_bsdnet_nameserver[rtems_bsdnet_nameserver_count]);
        rtems_bsdnet_nameserver_count++;
        dlen += 4;
      }
      }
      break;

    case 12:
      /* Host name */
      if (len>=MAXHOSTNAMELEN)
        panic ("bootpc: hostname >=%d bytes", MAXHOSTNAMELEN);
      if (sethostname (p, len) < 0)
        panic("Can't set host name");
      printf("Hostname is %s\n", p);
      break;

    case 7:
      /* Log servers */
      if (len % 4) 
        panic ("bootpc: Log server Len is %d", len);
      if (len > 0) {
        bcopy(p, &rtems_bsdnet_log_host_address, 4);
	dhcp_gotlogserver = 1;
      }
      break;

    case 15:
      /* Domain name */
      if (p[0]) {
        rtems_bsdnet_domain_name = strdup (p);
        printf("Domain name is %s\n", rtems_bsdnet_domain_name);
      }
      break;

    case 16:  /* Swap server IP address. unused */
      break;

    case 52:
      /* DHCP option override */
      if (len != 1) 
        panic ("bootpc: DHCP option overload len is %d", len);
      dhcpOptionOverload = p[0];
      break;

    case 128: /* Site-specific option for DHCP servers that 
               *   a) don't supply tag 54
               * and
               *   b) don't supply the server address in siaddr
               * For example, on Solaris 2.6 in.dhcpd, include in the dhcptab:
               *    Bootsrv s Site,128,IP,1,1
               * and use that symbol in the macro that defines the client:
               *    Bootsrv=<tftp-server-ip-address>
               */
    case 54:
      /* DHCP server */
      if (len != 4) 
        panic ("bootpc: DHCP server len is %d", len);
      bcopy(p, &rtems_bsdnet_bootp_server_address, 4);
      dhcp_gotserver = 1;
      break;

    case 66:
      /* DHCP server name option */
      if (p[0])
        rtems_bsdnet_bootp_server_name = strdup (p);
      break;

    case 67:
      /* DHCP bootfile option */
      if (p[0])
        rtems_bsdnet_bootp_boot_file_name = strdup (p);
      break;

    default:
      printf ("Ignoring BOOTP/DHCP option code %d\n", code);
      break;
    }
  }
}

#define EALEN 6

void
bootpc_init(void)
{
  struct bootp_packet call;
  struct bootp_packet reply;
  static u_int32_t xid = ~0xFF;
  
  struct ifreq ireq;
  struct ifnet *ifp;
  struct socket *so;
  int j;
  int error;
  struct sockaddr_in myaddr;
  struct ifaddr *ifa;
  struct sockaddr_dl *sdl = NULL;
  char *delim;
  struct proc *procp = NULL;

  /*
   * If already filled in, don't touch it here 
   */
  if (nfs_diskless_valid)
    return;

  /*
   * Find a network interface.
   */
  for (ifp = ifnet; ifp != 0; ifp = ifp->if_next)
    if ((ifp->if_flags &
      (IFF_LOOPBACK|IFF_POINTOPOINT)) == 0)
	break;
  if (ifp == NULL)
    panic("bootpc_init: no suitable interface");
  bzero(&ireq,sizeof(ireq));
  sprintf(ireq.ifr_name, "%s%d", ifp->if_name,ifp->if_unit);
  printf("bootpc_init: using network interface '%s'\n",
	 ireq.ifr_name);

  if ((error = socreate(AF_INET, &so, SOCK_DGRAM, 0,procp)) != 0)
    panic("nfs_boot: socreate, error=%d", error);
	  
  bootpc_fakeup_interface(&ireq,so,procp);

  printf("Bootpc testing starting\n");
  
  /* Get HW address */

  for (ifa = ifp->if_addrlist;ifa; ifa = ifa->ifa_next)
    if (ifa->ifa_addr->sa_family == AF_LINK &&
	(sdl = ((struct sockaddr_dl *) ifa->ifa_addr)) &&
	sdl->sdl_type == IFT_ETHER)
      break;
  
  if (!sdl)
    panic("bootpc: Unable to find HW address");
  if (sdl->sdl_alen != EALEN ) 
    panic("bootpc: HW address len is %d, expected value is %d",
	  sdl->sdl_alen,EALEN);

  printf("bootpc hw address is ");
  delim="";
  for (j=0;j<sdl->sdl_alen;j++) {
    printf("%s%x",delim,((unsigned char *)LLADDR(sdl))[j]);
    delim=":";
  }
  printf("\n");

#if 0
  bootpboot_p_iflist();
  bootpboot_p_rtlist();
#endif
  
  bzero((caddr_t) &call, sizeof(call));

  /* bootpc part */
  call.op = 1; 			/* BOOTREQUEST */
  call.htype= 1;		/* 10mb ethernet */
  call.hlen=sdl->sdl_alen;	/* Hardware address length */
  call.hops=0;	
  xid++;
  call.xid = txdr_unsigned(xid);
  bcopy(LLADDR(sdl),&call.chaddr,sdl->sdl_alen);
  
  call.vend[0]=99;
  call.vend[1]=130;
  call.vend[2]=83;
  call.vend[3]=99;
  call.vend[4]=255;
  
  call.secs = 0;
  call.flags = htons(0x8000); /* We need an broadcast answer */
  
  error = bootpc_call(&call,&reply,procp);
  
  if (error)
    panic("BOOTP call failed -- error %d", error);
  
  /*
   * Initialize network address structures
   */
  bzero(&myaddr,sizeof(myaddr));
  bzero(&dhcp_netmask,sizeof(dhcp_netmask));
  bzero(&dhcp_gw,sizeof(dhcp_gw));
  myaddr.sin_len = sizeof(myaddr);
  myaddr.sin_family = AF_INET;
  dhcp_netmask.sin_len = sizeof(dhcp_netmask);
  dhcp_netmask.sin_family = AF_INET;
  dhcp_gw.sin_len = sizeof(dhcp_gw);
  dhcp_gw.sin_family= AF_INET;

  /*
   * Set our address
   */
  myaddr.sin_addr = reply.yiaddr;
  printip("My ip address",myaddr.sin_addr);

  /*
   * Process BOOTP/DHCP options
   */
  if (reply.vend[0]==99 && reply.vend[1]==130 &&
      reply.vend[2]==83 && reply.vend[3]==99) {
    processOptions (&reply.vend[4], sizeof(reply.vend) - 4);
  }
  if (dhcpOptionOverload & 1) {
    processOptions (reply.file, sizeof reply.file);
  }
  else {
    if (reply.file[0])
      rtems_bsdnet_bootp_boot_file_name = strdup (reply.file);
  }
  if (dhcpOptionOverload & 2) {
    processOptions (reply.sname, sizeof reply.sname);
  }
  else {
    if (reply.sname[0])
      rtems_bsdnet_bootp_server_name = strdup (reply.sname);
  }
  if (rtems_bsdnet_bootp_server_name)
    printf ("Server name is %s\n", rtems_bsdnet_bootp_server_name);
  if (rtems_bsdnet_bootp_boot_file_name)
    printf ("Boot file is %s\n", rtems_bsdnet_bootp_boot_file_name);

  /*
   * Use defaults if values were not supplied by BOOTP/DHCP options
   */
  if (!dhcp_gotnetmask) {
    if (IN_CLASSA(ntohl(myaddr.sin_addr.s_addr)))
      dhcp_netmask.sin_addr.s_addr = htonl(IN_CLASSA_NET);
    else if (IN_CLASSB(ntohl(myaddr.sin_addr.s_addr)))
      dhcp_netmask.sin_addr.s_addr = htonl(IN_CLASSB_NET);
    else 
      dhcp_netmask.sin_addr.s_addr = htonl(IN_CLASSC_NET);
  }
  printip ("Subnet mask", dhcp_netmask.sin_addr);
  if (!dhcp_gotserver)
   rtems_bsdnet_bootp_server_address = reply.siaddr;
  printip ("Server ip address" ,rtems_bsdnet_bootp_server_address);
  if (!dhcp_gotgw)
    dhcp_gw.sin_addr = reply.giaddr;
  printip ("Gateway ip address", dhcp_gw.sin_addr);
  if (!dhcp_gotlogserver)
    rtems_bsdnet_log_host_address = rtems_bsdnet_bootp_server_address;
  printip ("Log server ip address", rtems_bsdnet_log_host_address);
  
  /*
   * Configure the interface with the new settings
   */
  error = bootpc_adjust_interface(&ireq,so,
				  &myaddr,&dhcp_netmask,&dhcp_gw,procp);
  soclose(so);
}

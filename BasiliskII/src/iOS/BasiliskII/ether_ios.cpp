/*
 *  ether_ios.cpp - Ethernet device driver, iOS specific stuff
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"
#include <dispatch/dispatch.h>

#define USE_POLL 1

// Define to let the slirp library determine the right timeout for select()
#define USE_SLIRP_TIMEOUT 1

#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <map>

#if defined(__FreeBSD__) || defined(sgi) || (defined(__APPLE__) && defined(__MACH__))
#include <net/if.h>
#endif

#ifdef HAVE_SLIRP
#include "libslirp.h"
#include "ctl.h"
#endif

#include "cpu_emulation.h"
#include "main.h"
#include "macos_util.h"
#include "prefs.h"
#include "user_strings.h"
#include "ether.h"
#include "ether_defs.h"

#ifndef NO_STD_NAMESPACE
using std::map;
#endif

#define DEBUG 0
#include "debug.h"

#define STATISTICS 0
#define MONITOR 0


// Ethernet device types
enum {
	NET_IF_SHEEPNET,
	NET_IF_ETHERTAP,
	NET_IF_TUNTAP,
	NET_IF_SLIRP
};

// Global variables
static int fd = -1;							// fd of sheep_net device
static pthread_t ether_thread;				// Packet reception thread
static pthread_attr_t ether_thread_attr;	// Packet reception thread attributes
static bool thread_active = false;			// Flag: Packet reception thread installed
static dispatch_semaphore_t int_ack;		// Interrupt acknowledge semaphore
static bool udp_tunnel;						// Flag: UDP tunnelling active, fd is the socket descriptor
static int net_if_type = -1;				// Ethernet device type
static pthread_t slirp_thread;				// Slirp reception thread
static bool slirp_thread_active = false;	// Flag: Slirp reception threadinstalled
static int slirp_output_fd = -1;			// fd of slirp output pipe
static int slirp_input_fds[2] = { -1, -1 };	// fds of slirp input pipe
#ifdef SHEEPSHAVER
static bool net_open = false;				// Flag: initialization succeeded, network device open
static uint8 ether_addr[6];					// Our Ethernet address
#else
const bool ether_driver_opened = true;		// Flag: is the MacOS driver opened?
#endif

// Attached network protocols, maps protocol type to MacOS handler address
static map<uint16, uint32> net_protocols;

// Prototypes
static void *receive_func(void *arg);
static void *slirp_receive_func(void *arg);
static int16 ether_do_add_multicast(uint8 *addr);
static int16 ether_do_del_multicast(uint8 *addr);
static int16 ether_do_write(uint32 arg);
static void ether_do_interrupt(void);
static void slirp_add_redirs();
static int slirp_add_redir(const char *redir_str);


/*
 *  Start packet reception thread
 */

static bool start_thread(void) {
	int_ack = dispatch_semaphore_create(0);
    if (int_ack == NULL) {
        perror("dispatch_semaphore_create");
        return false;
    }

	Set_pthread_attr(&ether_thread_attr, 1);
	thread_active = (pthread_create(&ether_thread, &ether_thread_attr, receive_func, NULL) == 0);
	if (!thread_active) {
		printf("WARNING: Cannot start Ethernet thread");
		return false;
	}

#ifdef HAVE_SLIRP
	if (net_if_type == NET_IF_SLIRP) {
		slirp_thread_active = (pthread_create(&slirp_thread, NULL, slirp_receive_func, NULL) == 0);
		if (!slirp_thread_active) {
			printf("WARNING: Cannot start slirp reception thread\n");
			return false;
		}
	}
#endif

	return true;
}


/*
 *  Stop packet reception thread
 */

static void stop_thread(void) {
#ifdef HAVE_SLIRP
	if (slirp_thread_active) {
#ifdef HAVE_PTHREAD_CANCEL
		pthread_cancel(slirp_thread);
#endif
		pthread_join(slirp_thread, NULL);
		slirp_thread_active = false;
	}
#endif

	if (thread_active) {
#ifdef HAVE_PTHREAD_CANCEL
		pthread_cancel(ether_thread);
#endif
		pthread_join(ether_thread, NULL);
		dispatch_release(int_ack);
		thread_active = false;
	}
}

/*
 *  Initialization
 */

bool ether_init(void) {
	int val;
	char str[256];

	// Do nothing if no Ethernet device specified
	const char *name = PrefsFindString("ether");
	if (name == NULL)
		return false;

	// Determine Ethernet device type
	net_if_type = -1;
#ifdef HAVE_SLIRP
    if (strcmp(name, "slirp") == 0)
		net_if_type = NET_IF_SLIRP;
#endif
	else
		net_if_type = NET_IF_SHEEPNET;

	// Don't raise SIGPIPE, let errno be set to EPIPE
	struct sigaction sigpipe_sa;
	if (sigaction(SIGPIPE, NULL, &sigpipe_sa) == 0) {
		assert(sigpipe_sa.sa_handler == SIG_DFL || sigpipe_sa.sa_handler == SIG_IGN);
		sigfillset(&sigpipe_sa.sa_mask);
		sigpipe_sa.sa_flags = 0;
		sigpipe_sa.sa_handler = SIG_IGN;
		sigaction(SIGPIPE, &sigpipe_sa, NULL);
	}

#ifdef HAVE_SLIRP
	// Initialize slirp library
	if (net_if_type == NET_IF_SLIRP) {
		if (slirp_init() < 0) {
			sprintf(str, "%s", GetString(STR_SLIRP_NO_DNS_FOUND_WARN));
			WarningAlert(str);
			return false;
		}

		// Open slirp output pipe
		int fds[2];
		if (pipe(fds) < 0)
			return false;
		fd = fds[0];
		slirp_output_fd = fds[1];

		// Open slirp input pipe
		if (pipe(slirp_input_fds) < 0)
			return false;

		// Set up port redirects
		slirp_add_redirs();
	}
#endif

	// Open sheep_net or ethertap or TUN/TAP device
	char dev_name[16];
	switch (net_if_type) {
	case NET_IF_ETHERTAP:
		sprintf(dev_name, "/dev/%s", name);
		break;
	case NET_IF_TUNTAP:
		strcpy(dev_name, "/dev/net/tun");
		break;
	case NET_IF_SHEEPNET:
		strcpy(dev_name, "/dev/sheep_net");
		break;
	}
	if (net_if_type != NET_IF_SLIRP) {
		fd = open(dev_name, O_RDWR);
		if (fd < 0) {
			sprintf(str, GetString(STR_NO_SHEEP_NET_DRIVER_WARN), dev_name, strerror(errno));
			WarningAlert(str);
			goto open_error;
		}
	}

	// Set nonblocking I/O
#ifdef USE_FIONBIO
	int nonblock = 1;
	if (ioctl(fd, FIONBIO, &nonblock) < 0) {
		sprintf(str, GetString(STR_BLOCKING_NET_SOCKET_WARN), strerror(errno));
		WarningAlert(str);
		goto open_error;
	}
#else
	val = fcntl(fd, F_GETFL, 0);
	if (val < 0 || fcntl(fd, F_SETFL, val | O_NONBLOCK) < 0) {
		sprintf(str, GetString(STR_BLOCKING_NET_SOCKET_WARN), strerror(errno));
		WarningAlert(str);
		goto open_error;
	}
#endif

	// Get Ethernet address
#ifdef HAVE_SLIRP
	if (net_if_type == NET_IF_SLIRP) {
		ether_addr[0] = 0x52;
		ether_addr[1] = 0x54;
		ether_addr[2] = 0x00;
		ether_addr[3] = 0x12;
		ether_addr[4] = 0x34;
		ether_addr[5] = 0x56;
	} else
#endif
		ioctl(fd, SIOCGIFADDR, ether_addr);
	D(bug("Ethernet address %02x %02x %02x %02x %02x %02x\n", ether_addr[0], ether_addr[1], ether_addr[2], ether_addr[3], ether_addr[4], ether_addr[5]));

	// Start packet reception thread
	if (!start_thread())
		goto open_error;

	// Everything OK
	return true;

open_error:
	stop_thread();

	if (fd > 0) {
		close(fd);
		fd = -1;
	}
	if (slirp_input_fds[0] >= 0) {
		close(slirp_input_fds[0]);
		slirp_input_fds[0] = -1;
	}
	if (slirp_input_fds[1] >= 0) {
		close(slirp_input_fds[1]);
		slirp_input_fds[1] = -1;
	}
	if (slirp_output_fd >= 0) {
		close(slirp_output_fd);
		slirp_output_fd = -1;
	}
	return false;
}


/*
 *  Deinitialization
 */

void ether_exit(void) {
	// Stop reception threads
	stop_thread();
	
	// Close slirp input buffer
	if (slirp_input_fds[0] >= 0)
		close(slirp_input_fds[0]);
	if (slirp_input_fds[1] >= 0)
		close(slirp_input_fds[1]);

	// Close slirp output buffer
	if (slirp_output_fd > 0)
		close(slirp_output_fd);

#if STATISTICS
	// Show statistics
	printf("%ld messages put on write queue\n", num_wput);
	printf("%ld error acks\n", num_error_acks);
	printf("%ld packets transmitted (%ld raw, %ld normal)\n", num_tx_packets, num_tx_raw_packets, num_tx_normal_packets);
	printf("%ld tx packets dropped because buffer full\n", num_tx_buffer_full);
	printf("%ld packets received\n", num_rx_packets);
	printf("%ld packets passed upstream (%ld Fast Path, %ld normal)\n", num_rx_fastpath + num_unitdata_ind, num_rx_fastpath, num_unitdata_ind);
	printf("EtherIRQ called %ld times\n", num_ether_irq);
	printf("%ld rx packets dropped due to low memory\n", num_rx_no_mem);
	printf("%ld rx packets dropped because no stream found\n", num_rx_dropped);
	printf("%ld rx packets dropped because stream not ready\n", num_rx_stream_not_ready);
	printf("%ld rx packets dropped because no memory for unitdata_ind\n", num_rx_no_unitdata_mem);
#endif
}


/*
 *  Glue around low-level implementation
 */

#ifdef SHEEPSHAVER
// Error codes
enum {
	eMultiErr		= -91,
	eLenErr			= -92,
	lapProtErr		= -94,
	excessCollsns	= -95
};

// Initialize ethernet
void EtherInit(void)
{
	net_open = false;

	// Do nothing if the user disabled the network
	if (PrefsFindBool("nonet"))
		return;

	net_open = ether_init();
}

// Exit ethernet
void EtherExit(void)
{
	ether_exit();
	net_open = false;
}

// Get ethernet hardware address
void AO_get_ethernet_address(uint32 arg)
{
	uint8 *addr = Mac2HostAddr(arg);
	if (net_open)
		OTCopy48BitAddress(ether_addr, addr);
	else {
		addr[0] = 0x12;
		addr[1] = 0x34;
		addr[2] = 0x56;
		addr[3] = 0x78;
		addr[4] = 0x9a;
		addr[5] = 0xbc;
	}
	D(bug("AO_get_ethernet_address: got address %02x%02x%02x%02x%02x%02x\n", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]));
}

// Add multicast address
void AO_enable_multicast(uint32 addr)
{
	if (net_open)
		ether_do_add_multicast(Mac2HostAddr(addr));
}

// Disable multicast address
void AO_disable_multicast(uint32 addr)
{
	if (net_open)
		ether_do_del_multicast(Mac2HostAddr(addr));
}

// Transmit one packet
void AO_transmit_packet(uint32 mp)
{
	if (net_open) {
		switch (ether_do_write(mp)) {
		case noErr:
			num_tx_packets++;
			break;
		case excessCollsns:
			num_tx_buffer_full++;
			break;
		}
	}
}

// Copy packet data from message block to linear buffer
static inline int ether_arg_to_buffer(uint32 mp, uint8 *p)
{
	return ether_msgb_to_buffer(mp, p);
}

// Ethernet interrupt
void EtherIRQ(void)
{
	D(bug("EtherIRQ\n"));
	num_ether_irq++;

	OTEnterInterrupt();
	ether_do_interrupt();
	OTLeaveInterrupt();

	// Acknowledge interrupt to reception thread
	D(bug(" EtherIRQ done\n"));
	dispatch_semaphore_signal(int_ack);
}
#else
// Add multicast address
int16 ether_add_multicast(uint32 pb)
{
	return ether_do_add_multicast(Mac2HostAddr(pb + eMultiAddr));
}

// Disable multicast address
int16 ether_del_multicast(uint32 pb)
{
	return ether_do_del_multicast(Mac2HostAddr(pb + eMultiAddr));
}

// Transmit one packet
int16 ether_write(uint32 wds)
{
	return ether_do_write(wds);
}

// Copy packet data from WDS to linear buffer
static inline int ether_arg_to_buffer(uint32 wds, uint8 *p)
{
	return ether_wds_to_buffer(wds, p);
}

// Dispatch packet to protocol handler
static void ether_dispatch_packet(uint32 p, uint32 length)
{
	// Get packet type
	uint16 type = ReadMacInt16(p + 12);

	// Look for protocol
	uint16 search_type = (type <= 1500 ? 0 : type);
	if (net_protocols.find(search_type) == net_protocols.end())
		return;
	uint32 handler = net_protocols[search_type];

	// No default handler
	if (handler == 0)
		return;

	// Copy header to RHA
	Mac2Mac_memcpy(ether_data + ed_RHA, p, 14);
	D(bug(" header %08x%04x %08x%04x %04x\n", ReadMacInt32(ether_data + ed_RHA), ReadMacInt16(ether_data + ed_RHA + 4), ReadMacInt32(ether_data + ed_RHA + 6), ReadMacInt16(ether_data + ed_RHA + 10), ReadMacInt16(ether_data + ed_RHA + 12)));

	// Call protocol handler
	M68kRegisters r;
	r.d[0] = type;									// Packet type
	r.d[1] = length - 14;							// Remaining packet length (without header, for ReadPacket)
	r.a[0] = p + 14;								// Pointer to packet (Mac address, for ReadPacket)
	r.a[3] = ether_data + ed_RHA + 14;				// Pointer behind header in RHA
	r.a[4] = ether_data + ed_ReadPacket;			// Pointer to ReadPacket/ReadRest routines
	D(bug(" calling protocol handler %08x, type %08x, length %08x, data %08x, rha %08x, read_packet %08x\n", handler, r.d[0], r.d[1], r.a[0], r.a[3], r.a[4]));
	Execute68k(handler, &r);
}

// Ethernet interrupt
void EtherInterrupt(void)
{
	D(bug("EtherIRQ\n"));
	ether_do_interrupt();

	// Acknowledge interrupt to reception thread
	D(bug(" EtherIRQ done\n"));
	dispatch_semaphore_signal(int_ack);
}
#endif


/*
 *  Reset
 */

void ether_reset(void)
{
	net_protocols.clear();
}


/*
 *  Add multicast address
 */

static int16 ether_do_add_multicast(uint8 *addr)
{
	switch (net_if_type) {
	case NET_IF_ETHERTAP:
	case NET_IF_SHEEPNET:
		if (ioctl(fd, SIOCADDMULTI, addr) < 0) {
			D(bug("WARNING: Couldn't enable multicast address\n"));
			if (net_if_type == NET_IF_ETHERTAP) {
				return noErr;
			} else {
				return eMultiErr;
			}
		}
		return noErr;
	default:
		return noErr;
	}
}


/*
 *  Delete multicast address
 */

static int16 ether_do_del_multicast(uint8 *addr)
{
	switch (net_if_type) {
	case NET_IF_ETHERTAP:
	case NET_IF_SHEEPNET:
		if (ioctl(fd, SIOCDELMULTI, addr) < 0) {
			D(bug("WARNING: Couldn't disable multicast address\n"));
			return eMultiErr;
		}
		return noErr;
	default:
		return noErr;
	}
}


/*
 *  Attach protocol handler
 */

int16 ether_attach_ph(uint16 type, uint32 handler)
{
	if (net_protocols.find(type) != net_protocols.end())
		return lapProtErr;
	net_protocols[type] = handler;
	return noErr;
}


/*
 *  Detach protocol handler
 */

int16 ether_detach_ph(uint16 type)
{
	if (net_protocols.erase(type) == 0)
		return lapProtErr;
	return noErr;
}


/*
 *  Transmit raw ethernet packet
 */

static int16 ether_do_write(uint32 arg)
{
	// Copy packet to buffer
	uint8 packet[1516], *p = packet;
	int len = 0;
	len += ether_arg_to_buffer(arg, p);

#if MONITOR
	bug("Sending Ethernet packet:\n");
	for (int i=0; i<len; i++) {
		bug("%02x ", packet[i]);
	}
	bug("\n");
#endif

	// Transmit packet
#ifdef HAVE_SLIRP
	if (net_if_type == NET_IF_SLIRP) {
		const int slirp_input_fd = slirp_input_fds[1];
		write(slirp_input_fd, &len, sizeof(len));
		write(slirp_input_fd, packet, len);
		return noErr;
	} else
#endif
	if (write(fd, packet, len) < 0) {
		D(bug("WARNING: Couldn't transmit packet\n"));
		return excessCollsns;
	} else
		return noErr;
}


/*
 *  Start UDP packet reception thread
 */

bool ether_start_udp_thread(int socket_fd)
{
	fd = socket_fd;
	udp_tunnel = true;
	return start_thread();
}


/*
 *  Stop UDP packet reception thread
 */

void ether_stop_udp_thread(void)
{
	stop_thread();
	fd = -1;
}


/*
 *  SLIRP output buffer glue
 */

#ifdef HAVE_SLIRP
int slirp_can_output(void)
{
	return 1;
}

void slirp_output(const uint8 *packet, int len)
{
	write(slirp_output_fd, packet, len);
}

void *slirp_receive_func(void *arg)
{
	const int slirp_input_fd = slirp_input_fds[0];

	for (;;) {
		// Wait for packets to arrive
		fd_set rfds, wfds, xfds;
		int nfds;
		struct timeval tv;

		// ... in the input queue
		FD_ZERO(&rfds);
		FD_SET(slirp_input_fd, &rfds);
		tv.tv_sec = 0;
		tv.tv_usec = 0;
		if (select(slirp_input_fd + 1, &rfds, NULL, NULL, &tv) > 0) {
			int len;
			read(slirp_input_fd, &len, sizeof(len));
			uint8 packet[1516];
			assert(len <= sizeof(packet));
			read(slirp_input_fd, packet, len);
			slirp_input(packet, len);
		}

		// ... in the output queue
		nfds = -1;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_ZERO(&xfds);
		int timeout = slirp_select_fill(&nfds, &rfds, &wfds, &xfds);
#if ! USE_SLIRP_TIMEOUT
		timeout = 10000;
#endif
		tv.tv_sec = 0;
		tv.tv_usec = timeout;
		if (select(nfds + 1, &rfds, &wfds, &xfds, &tv) >= 0)
			slirp_select_poll(&rfds, &wfds, &xfds);

#ifdef HAVE_PTHREAD_TESTCANCEL
		// Explicit cancellation point if select() was not covered
		// This seems to be the case on MacOS X 10.2
		pthread_testcancel();
#endif
	}
	return NULL;
}
#else
int slirp_can_output(void)
{
	return 0;
}

void slirp_output(const uint8 *packet, int len)
{
}
#endif


/*
 *  Packet reception thread
 */

static void *receive_func(void *arg)
{
	for (;;) {

		// Wait for packets to arrive
#if USE_POLL
		struct pollfd pf = {fd, POLLIN, 0};
		int res = poll(&pf, 1, -1);
#else
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		// A NULL timeout could cause select() to block indefinitely,
		// even if it is supposed to be a cancellation point [MacOS X]
		struct timeval tv = { 0, 20000 };
		int res = select(fd + 1, &rfds, NULL, NULL, &tv);
#ifdef HAVE_PTHREAD_TESTCANCEL
		pthread_testcancel();
#endif
		if (res == 0 || (res == -1 && errno == EINTR))
			continue;
#endif
		if (res <= 0)
			break;

		if (ether_driver_opened) {
			// Trigger Ethernet interrupt
			D(bug(" packet received, triggering Ethernet interrupt\n"));
			SetInterruptFlag(INTFLAG_ETHER);
			TriggerInterrupt();

			// Wait for interrupt acknowledge by EtherInterrupt()
			dispatch_semaphore_wait(int_ack, DISPATCH_TIME_FOREVER);
		} else
			Delay_usec(20000);
	}
	return NULL;
}


/*
 *  Ethernet interrupt - activate deferred tasks to call IODone or protocol handlers
 */

void ether_do_interrupt(void)
{
	// Call protocol handler for received packets
	EthernetPacket ether_packet;
	uint32 packet = ether_packet.addr();
	ssize_t length;
	for (;;) {

#ifndef SHEEPSHAVER
		if (udp_tunnel) {

			// Read packet from socket
			struct sockaddr_in from;
			socklen_t from_len = sizeof(from);
			length = recvfrom(fd, Mac2HostAddr(packet), 1514, 0, (struct sockaddr *)&from, &from_len);
			if (length < 14)
				break;
			ether_udp_read(packet, length, &from);

		} else
#endif
		{

			// Read packet from sheep_net device
			length = read(fd, Mac2HostAddr(packet), 1514);
			if (length < 14)
				break;

#if MONITOR
			bug("Receiving Ethernet packet:\n");
			for (int i=0; i<length; i++) {
				bug("%02x ", ReadMacInt8(packet + i));
			}
			bug("\n");
#endif

			// Pointer to packet data (Ethernet header)
			uint32 p = packet;
			// Dispatch packet
			ether_dispatch_packet(p, length);
		}
	}
}

// Helper function for port forwarding
static int get_str_sep(char *buf, int buf_size, const char **pp, int sep)
{
	const char *p, *p1;
	int len;
	p = *pp;
	p1 = strchr(p, sep);
	if (!p1)
		return -1;
	len = p1 - p;
	p1++;
	if (buf_size > 0) {
		if (len > buf_size - 1)
			len = buf_size - 1;
		memcpy(buf, p, len);
		buf[len] = '\0';
	}
	*pp = p1;
	return 0;
}

// Set up port forwarding for slirp
static void slirp_add_redirs()
{
	int index = 0;
	const char *str;
	while ((str = PrefsFindString("redir", index++)) != NULL) {
		slirp_add_redir(str);
	}
}

// Add a port forward/redirection for slirp
static int slirp_add_redir(const char *redir_str)
{
	// code adapted from qemu source
	struct in_addr guest_addr = {0};
	int host_port, guest_port;
	const char *p;
	char buf[256];
	int is_udp;
	char *end;
	char str[256];

	p = redir_str;
	if (!p || get_str_sep(buf, sizeof(buf), &p, ':') < 0) {
		goto fail_syntax;
	}
	if (!strcmp(buf, "tcp") || buf[0] == '\0') {
		is_udp = 0;
	} else if (!strcmp(buf, "udp")) {
		is_udp = 1;
	} else {
		goto fail_syntax;
	}

	if (get_str_sep(buf, sizeof(buf), &p, ':') < 0) {
		goto fail_syntax;
	}
	host_port = strtol(buf, &end, 0);
	if (*end != '\0' || host_port < 1 || host_port > 65535) {
		goto fail_syntax;
	}

	if (get_str_sep(buf, sizeof(buf), &p, ':') < 0) {
		goto fail_syntax;
	}
	// 0.0.0.0 doesn't seem to work, so default to a client address
	// if none is specified
	if (buf[0] == '\0' ?
			!inet_aton(CTL_LOCAL, &guest_addr) :
			!inet_aton(buf, &guest_addr)) {
		goto fail_syntax;
	}

	guest_port = strtol(p, &end, 0);
	if (*end != '\0' || guest_port < 1 || guest_port > 65535) {
		goto fail_syntax;
	}

	if (slirp_redir(is_udp, host_port, guest_addr, guest_port) < 0) {
		sprintf(str, "could not set up host forwarding rule '%s'", redir_str);
		WarningAlert(str);
		return -1;
	}
	return 0;

 fail_syntax:
	sprintf(str, "invalid host forwarding rule '%s'", redir_str);
	WarningAlert(str);
	return -1;
}

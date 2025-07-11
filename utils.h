// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Felix Fietkau <nbd@nbd.name>
 */
#ifndef __UNETD_UTILS_H
#define __UNETD_UTILS_H

#if defined(__linux__) || defined(__CYGWIN__)
#include <byteswap.h>
#include <endian.h>

#elif defined(__APPLE__)
#include <machine/endian.h>
#include <machine/byte_order.h>
#elif defined(__FreeBSD__)
#include <sys/endian.h>
#else
#include <machine/endian.h>
#endif

#include <string.h>
#include <netinet/in.h>
#include <libubox/utils.h>

struct nl_msg;

union network_addr {
	struct {
		uint8_t network_id[8];
		uint8_t host_addr[8];
	};
	struct in_addr in;
	struct in6_addr in6;
};

union network_endpoint {
	struct sockaddr sa;
	struct sockaddr_in in;
	struct sockaddr_in6 in6;
};

static inline void *
network_endpoint_addr(union network_endpoint *ep, int *addr_len)
{
	if (ep->sa.sa_family == AF_INET6) {
		if (addr_len)
			*addr_len = sizeof(ep->in6.sin6_addr);
		return &ep->in6.sin6_addr;
	}

	if (addr_len)
		*addr_len = sizeof(ep->in.sin_addr);
	return &ep->in.sin_addr;
}

static inline bool
network_endpoint_addr_equal(union network_endpoint *ep1, union network_endpoint *ep2)
{
	const void *a1, *a2;
	int len;

	if (ep1->sa.sa_family != ep2->sa.sa_family)
		return false;

	a1 = network_endpoint_addr(ep1, &len);
	a2 = network_endpoint_addr(ep2, &len);

	return !memcmp(a1, a2, len);
}

int network_get_endpoint(union network_endpoint *dest, int af, const char *str,
			 int default_port, int idx);
int network_get_subnet(int af, union network_addr *addr, int *mask,
		       const char *str);
int network_get_local_addr(void *local, const union network_endpoint *target);

void *unet_read_file(const char *name, size_t *len);

#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))

#define bitmask_size(len)	(4 * DIV_ROUND_UP(len, 32))

static inline bool bitmask_test(uint32_t *mask, unsigned int i)
{
	return mask[i / 32] & (1 << (i % 32));
}

static inline void bitmask_set(uint32_t *mask, unsigned int i)
{
	mask[i / 32] |= 1 << (i % 32);
}

static inline void bitmask_clear(uint32_t *mask, unsigned int i)
{
	mask[i / 32] &= ~(1 << (i % 32));
}

static inline void bitmask_set_val(uint32_t *mask, unsigned int i, bool val)
{
	if (val)
		bitmask_set(mask, i);
	else
		bitmask_clear(mask, i);
}

static inline uint16_t get_unaligned_be16(const uint8_t *p)
{
	return p[1] | p[0] << 8;
}

static inline uint32_t get_unaligned_be32(const uint8_t *p)
{
	return p[3] | p[2] << 8 | p[1] << 16 | p[0] << 24;
}

static inline uint64_t get_unaligned_be64(const uint8_t *p)
{
	return (uint64_t)get_unaligned_be32(p) << 32 |
	       get_unaligned_be32(p + 4);
}

static inline uint16_t get_unaligned_le16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

static inline uint32_t get_unaligned_le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
}

static inline uint64_t get_unaligned_le64(const uint8_t *p)
{
	return (uint64_t)get_unaligned_le32(p + 4) << 32 |
	       get_unaligned_le32(p);
}

int rtnl_init(void);
int rtnl_call(struct nl_msg *msg);

uint64_t unet_gettime(void);

int sendto_rawudp(int fd, const void *addr, void *ip_hdr, size_t ip_hdrlen,
		  const void *data, size_t len);

#ifdef __GNUC__
#define _GNUC_MIN_VER(maj, min) (((__GNUC__ << 8) + __GNUC_MINOR__) >= (((maj) << 8) + (min)))
#else
#define _GNUC_MIN_VER(maj, min) 0
#endif


#ifndef __BYTE_ORDER
#define __BYTE_ORDER BYTE_ORDER
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN BIG_ENDIAN
#endif
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN LITTLE_ENDIAN
#endif

#define __constant_swap16(x) ((uint16_t)(				\
	(((uint16_t)(x) & (uint16_t)0x00ffU) << 8) |			\
	(((uint16_t)(x) & (uint16_t)0xff00U) >> 8)))

#define __constant_swap32(x) ((uint32_t)(				\
	(((uint32_t)(x) & (uint32_t)0x000000ffUL) << 24) |		\
	(((uint32_t)(x) & (uint32_t)0x0000ff00UL) <<  8) |		\
	(((uint32_t)(x) & (uint32_t)0x00ff0000UL) >>  8) |		\
	(((uint32_t)(x) & (uint32_t)0xff000000UL) >> 24)))

#define __constant_swap64(x) ((uint64_t)(				\
	(((uint64_t)(x) & (uint64_t)0x00000000000000ffULL) << 56) |	\
	(((uint64_t)(x) & (uint64_t)0x000000000000ff00ULL) << 40) |	\
	(((uint64_t)(x) & (uint64_t)0x0000000000ff0000ULL) << 24) |	\
	(((uint64_t)(x) & (uint64_t)0x00000000ff000000ULL) <<  8) |	\
	(((uint64_t)(x) & (uint64_t)0x000000ff00000000ULL) >>  8) |	\
	(((uint64_t)(x) & (uint64_t)0x0000ff0000000000ULL) >> 24) |	\
	(((uint64_t)(x) & (uint64_t)0x00ff000000000000ULL) >> 40) |	\
	(((uint64_t)(x) & (uint64_t)0xff00000000000000ULL) >> 56)))

/*
 * This returns a constant expression while determining if an argument is
 * a constant expression, most importantly without evaluating the argument.
 */
#define __is_constant(x)						\
	(sizeof(int) == sizeof(*(1 ? ((void*)((long)(x) * 0l)) : (int*)1)))

#define __eval_once(func, x)						\
	({ __typeof__(x) __x = x; func(__x); })

#ifdef __cplusplus
/*
 * g++ does not support __builtin_choose_expr, so always use __eval_once.
 * Unfortunately this means that the byte order functions can't be used
 * as a constant expression anymore
 */
#define __eval_safe(func, x) __eval_once(func, x)
#else
#define __eval_safe(func, x)						\
	__builtin_choose_expr(__is_constant(x),				\
			      func(x), __eval_once(func, x))
#endif

#if __BYTE_ORDER == __LITTLE_ENDIAN

#define const_cpu_to_be64(x) __constant_swap64(x)
#define const_cpu_to_be32(x) __constant_swap32(x)
#define const_cpu_to_be16(x) __constant_swap16(x)

#define const_be64_to_cpu(x) __constant_swap64(x)
#define const_be32_to_cpu(x) __constant_swap32(x)
#define const_be16_to_cpu(x) __constant_swap16(x)

#define const_cpu_to_le64(x) (x)
#define const_cpu_to_le32(x) (x)
#define const_cpu_to_le16(x) (x)

#define const_le64_to_cpu(x) (x)
#define const_le32_to_cpu(x) (x)
#define const_le16_to_cpu(x) (x)

#define cpu_to_be64(x) __eval_safe(__constant_swap64, x)
#define cpu_to_be32(x) __eval_safe(__constant_swap32, x)
#define cpu_to_be16(x) __eval_safe(__constant_swap16, x)

#define be64_to_cpu(x) __eval_safe(__constant_swap64, x)
#define be32_to_cpu(x) __eval_safe(__constant_swap32, x)
#define be16_to_cpu(x) __eval_safe(__constant_swap16, x)

#define cpu_to_le64(x) (x)
#define cpu_to_le32(x) (x)
#define cpu_to_le16(x) (x)

#define le64_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define le16_to_cpu(x) (x)

#else /* __BYTE_ORDER == __LITTLE_ENDIAN */

#define const_cpu_to_le64(x) __constant_swap64(x)
#define const_cpu_to_le32(x) __constant_swap32(x)
#define const_cpu_to_le16(x) __constant_swap16(x)

#define const_le64_to_cpu(x) __constant_swap64(x)
#define const_le32_to_cpu(x) __constant_swap32(x)
#define const_le16_to_cpu(x) __constant_swap16(x)

#define const_cpu_to_be64(x) (x)
#define const_cpu_to_be32(x) (x)
#define const_cpu_to_be16(x) (x)

#define const_be64_to_cpu(x) (x)
#define const_be32_to_cpu(x) (x)
#define const_be16_to_cpu(x) (x)

#define cpu_to_le64(x) __eval_safe(__constant_swap64, x)
#define cpu_to_le32(x) __eval_safe(__constant_swap32, x)
#define cpu_to_le16(x) __eval_safe(__constant_swap16, x)

#define le64_to_cpu(x) __eval_safe(__constant_swap64, x)
#define le32_to_cpu(x) __eval_safe(__constant_swap32, x)
#define le16_to_cpu(x) __eval_safe(__constant_swap16, x)

#define cpu_to_be64(x) (x)
#define cpu_to_be32(x) (x)
#define cpu_to_be16(x) (x)

#define be64_to_cpu(x) (x)
#define be32_to_cpu(x) (x)
#define be16_to_cpu(x) (x)

#endif

#ifndef fallthrough
# if __has_attribute(__fallthrough__)
#  define fallthrough __attribute__((__fallthrough__))
# else
#  define fallthrough do {} while (0)  /* fallthrough */
# endif
#endif

#endif

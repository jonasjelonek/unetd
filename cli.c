// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Felix Fietkau <nbd@nbd.name>
 */
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>
#include <unistd.h>
#include <libubox/utils.h>
#include <libubox/uloop.h>
#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include "edsign.h"
#include "ed25519.h"
#include "curve25519.h"
#include "auth-data.h"
#include "random.h"
#include "pex-msg.h"
#include "sntrup761.h"
#include "mldsa.h"

#define _max(a, b) ((a) > (b) ? (a) : (b))

#define MAX_PUBKEY_SIZE _max(MLD_44_PUBLICKEYBYTES, SNTRUP761_PUB_SIZE)
#define MAX_SECKEY_SIZE _max(MLD_44_SECRETKEYBYTES, SNTRUP761_SEC_SIZE)
#define MAX_KEY_SIZE _max(MAX_PUBKEY_SIZE, MAX_SECKEY_SIZE)

static uint8_t peerkey[EDSIGN_PUBLIC_KEY_SIZE];
static uint8_t pubkey[EDSIGN_PUBLIC_KEY_SIZE];
static uint8_t seckey[EDSIGN_PUBLIC_KEY_SIZE];
static uint8_t xorkey[EDSIGN_PUBLIC_KEY_SIZE];
static uint8_t pubkey_pq[MAX_SECKEY_SIZE];
static uint8_t seckey_pq[MAX_SECKEY_SIZE];
static uint8_t xorkey_pq[MAX_SECKEY_SIZE];
static size_t pubkey_pq_len, seckey_pq_len, xorkey_pq_len;
static void *net_data;
static size_t net_data_len;
static uint64_t net_data_version;
static struct blob_attr *net_data_hosts;
static uint64_t req_id;
static struct blob_buf b;
static FILE *out_file;
static bool quiet;
static bool sync_done;
static bool pq_keys;
static bool has_key, has_xor;
static int password_prompt;
static enum {
	CMD_UNKNOWN,
	CMD_GENERATE_SIGN,
	CMD_GENERATE_HOST,
	CMD_PUBKEY,
	CMD_HOST_PUBKEY,
	CMD_VERIFY,
	CMD_SIGN,
	CMD_DOWNLOAD,
	CMD_UPLOAD,
	CMD_NETDATA,
} cmd;

#define INFO(...)					\
	do {						\
		if (quiet)				\
			break;				\
		fprintf(stderr, ##__VA_ARGS__);		\
	} while (0)

static void print_key(const uint8_t *key, const uint8_t *pq_key, size_t pq_len)
{
	char keystr[B64_ENCODE_LEN(MAX_KEY_SIZE)];

	if (b64_encode(key, EDSIGN_PUBLIC_KEY_SIZE, keystr, sizeof(keystr)) < 0)
		return;

	if (pq_key && pq_len) {
		fprintf(out_file, "%s:", keystr);

		if (b64_encode(pq_key, pq_len, keystr, sizeof(keystr)) < 0)
			return;
	}

	fprintf(out_file, "%s\n", keystr);
}

static int usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [command|options] [<file>]\n"
		"Commands:\n"
		"	-S			Sign file\n"
		"	-V			Verify file\n"
		"	-P			Get public signing key from secret key\n"
		"	-H			Get public host key from secret key\n"
		"	-T			Get network data from signed file\n"
		"	-g			Generate new private host key\n"
		"	-G			Generate new private signing key\n"
		"	-D <host>[:<port>]	Download network data from unetd\n"
		"	-U <host>[:<port>]	Upload network data to unetd\n"
		"\n"
		"Options:\n"
		"	-q:			Quiet mode - suppress error/info messages\n"
		"	-o <file>:		Set output file to <file> (defaults to stdout)\n"
		"	-k <keyfile>|-:		Set public key from file or stdin\n"
		"	-K <keyfile>|-:		Set secret key from file or stdin\n"
		"	-h <keyfile>|-		Set peer private key from file or stdin\n"
		"				(for network data down-/upload)\n"
		"	-s <n>,<salt>		Generating secret key from seed using <n> rounds and <salt>\n"
		"				(passphrase is read from stdin)\n"
		"	-p			Prompt for seed password\n"
		"	-b <file>:		Read signed network data file\n"
		"	-x <file>|-:		Apply extra key using XOR\n"
		"	-Q			Enable post-quantum keys\n"
		"\n", progname);
	return 1;
}

static void pex_timeout(struct uloop_timeout *timeout)
{
	uloop_end();
}

static void
pex_recv_update_response(const uint8_t *data, size_t len, enum pex_opcode op)
{
	int net_data_len = 0;
	void *net_data;

	net_data = pex_msg_update_response_recv(data, len, op, &net_data_len, NULL);
	if (net_data_len < 0)
		goto out;

	if (!net_data)
		return;

	if (cmd == CMD_DOWNLOAD) {
		fwrite(net_data, net_data_len, 1, out_file);
		sync_done = true;
	}

	free(net_data);

out:
	if (cmd == CMD_DOWNLOAD)
		uloop_end();
}

static bool
pex_get_pubkey(uint8_t *pubkey, const uint8_t *id)
{
	static const struct blobmsg_policy policy = { "key", BLOBMSG_TYPE_STRING };
	struct blob_attr *cur, *key;
	int rem;

	blobmsg_for_each_attr(cur, net_data_hosts, rem) {
		const char *keystr;

		blobmsg_parse(&policy, 1, &key, blobmsg_data(cur), blobmsg_len(cur));

		if (!key)
			continue;

		keystr = blobmsg_get_string(key);
		if (b64_decode(keystr, pubkey, CURVE25519_KEY_SIZE) != CURVE25519_KEY_SIZE)
			continue;

		if (!memcmp(pubkey, id, PEX_ID_LEN))
			return true;
	}

	return false;
}

static void
pex_handle_update_request(struct sockaddr_in6 *addr, const uint8_t *id, void *data, size_t len)
{
	struct pex_msg_update_send_ctx ctx = {};
	static uint8_t empty_key[EDSIGN_PUBLIC_KEY_SIZE] = {};
	uint8_t peerpubkey[EDSIGN_PUBLIC_KEY_SIZE];
	bool done = false;

	if (!pex_get_pubkey(peerpubkey, id)) {
		INFO("Could not find public key\n");
		return;
	}

	pex_msg_update_response_init(&ctx, empty_key, pubkey,
				     peerpubkey, true, data, net_data, net_data_len);
	while (!done) {
		__pex_msg_send(-1, NULL, NULL, 0);
		done = !pex_msg_update_response_continue(&ctx);
	}
	sync_done = true;
	uloop_end();
}

static void pex_recv(void *msg, size_t msg_len, struct sockaddr_in6 *addr)
{
	struct pex_hdr *hdr;
	struct pex_ext_hdr *ehdr;
	uint64_t *msg_req_id;
	void *data;

	hdr = pex_rx_accept(msg, msg_len, true);
	if (!hdr)
		return;

	ehdr = (void *)(hdr + 1);
	data = (void *)(ehdr + 1);
	msg_req_id = data;

	if (hdr->version != 0)
		return;

	if (memcmp(ehdr->auth_id, pubkey, sizeof(ehdr->auth_id)) != 0)
		return;

	*(uint64_t *)hdr->id ^= pex_network_hash(pubkey, ehdr->nonce);

	switch (hdr->opcode) {
	case PEX_MSG_UPDATE_REQUEST:
		if (cmd != CMD_UPLOAD)
			break;

		pex_handle_update_request(addr, hdr->id, data, hdr->len);
		break;
	case PEX_MSG_UPDATE_RESPONSE:
	case PEX_MSG_UPDATE_RESPONSE_DATA:
	case PEX_MSG_UPDATE_RESPONSE_NO_DATA:
		if (hdr->len < sizeof(*msg_req_id) || *msg_req_id != req_id)
			break;

		if (cmd == CMD_DOWNLOAD &&
		    hdr->opcode == PEX_MSG_UPDATE_RESPONSE_NO_DATA) {
			INFO("No network data available\n");
			uloop_end();
		}

		if (cmd == CMD_UPLOAD &&
		    hdr->opcode != PEX_MSG_UPDATE_RESPONSE_NO_DATA) {
			INFO("Server has newer network data\n");
			uloop_end();
		}

		pex_recv_update_response(data, hdr->len, hdr->opcode);
		break;
	}
}

static int load_network_data(const char *file)
{
	static const struct blobmsg_policy policy = { "hosts", BLOBMSG_TYPE_TABLE };
	struct unet_auth_hdr *hdr;
	struct unet_auth_data *data;
	const char *json;

	net_data_len = UNETD_NET_DATA_SIZE_MAX;
	net_data = unet_read_file(file, &net_data_len);
	if (!net_data) {
		INFO("failed to read input file %s\n", file);
		return 1;
	}

	if (unet_auth_data_validate(NULL, net_data, net_data_len, &net_data_version, &json) < 0) {
		INFO("input data validation failed\n");
		return 1;
	}

	hdr = net_data;
	data = (struct unet_auth_data *)(hdr + 1);
	memcpy(pubkey, data->pubkey, sizeof(data->pubkey));

	blob_buf_init(&b, 0);
	blobmsg_add_json_from_string(&b, json);

	blobmsg_parse(&policy, 1, &net_data_hosts, blobmsg_data(b.head), blobmsg_len(b.head));
	if (!net_data_hosts) {
		INFO("network data is missing the hosts attribute\n");
		return 1;
	}

	return 0;
}


static int cmd_sync(const char *endpoint, int argc, char **argv)
{
	uint8_t peerpubkey[EDSIGN_PUBLIC_KEY_SIZE];
	struct uloop_timeout timeout = {
		.cb = pex_timeout
	};
	struct pex_update_request *req;
	union network_endpoint ep = {};
	int len;

	if (cmd == CMD_UPLOAD) {
		if (argc < 1) {
			INFO("missing file argument\n");
			return 1;
		}

		if (load_network_data(argv[0]))
			return 1;
	}

	if (network_get_endpoint(&ep, AF_UNSPEC, endpoint, UNETD_GLOBAL_PEX_PORT, 0) < 0) {
		INFO("Invalid hostname/port %s\n", endpoint);
		return 1;
	}

	len = ep.sa.sa_family == AF_INET6 ? sizeof(ep.in6) : sizeof(ep.in);

	uloop_init();

	if (pex_open(&ep, len, pex_recv, false) < 0)
		return 1;

	uloop_timeout_set(&timeout, 5000);

	curve25519_generate_public(peerpubkey, peerkey);
	req = pex_msg_update_request_init(peerpubkey, peerkey, pubkey, &ep,
					  net_data_version, true);
	if (!req)
		return 1;

	req_id = req->req_id;
	if (__pex_msg_send(-1, NULL, NULL, 0) < 0) {
		if (!quiet)
			perror("send");
		return 1;
	}

	uloop_run();

	return !sync_done;
}

static int cmd_sign(int argc, char **argv)
{
	struct unet_auth_hdr hdr = {
		.magic = cpu_to_be32(UNET_AUTH_MAGIC),
	};
	struct unet_auth_data *data;
	struct timeval tv;
	struct stat st;
	off_t len;
	FILE *f;

	if (argc != 1) {
		INFO("Missing filename\n");
		return 1;
	}

	if (gettimeofday(&tv, NULL)) {
		if (!quiet)
			perror("gettimeofday");
		return 1;
	}

	if (stat(argv[0], &st) ||
	    (f = fopen(argv[0], "r")) == NULL) {
		INFO("Input file not found\n");
		return 1;
	}

	data = calloc(1, sizeof(*data) + st.st_size + 1);
	data->timestamp = cpu_to_be64(tv.tv_sec);
	len = fread(data + 1, 1, st.st_size, f);
	fclose(f);

	if (len != st.st_size) {
		INFO("Error reading from input file\n");
		return 1;
	}

	len += sizeof(*data) + 1;

	memcpy(data->pubkey, pubkey, sizeof(data->pubkey));
	edsign_sign(hdr.signature, pubkey, seckey, (const void *)data, len);

	fwrite(&hdr, sizeof(hdr), 1, out_file);
	fwrite(data, len, 1, out_file);

	free(data);

	return 0;
}

static int cmd_verify(int argc, char **argv)
{
	struct unet_auth_data *data;
	struct unet_auth_hdr *hdr;
	struct stat st;
	off_t len;
	FILE *f;
	int ret = 1;

	if (argc != 1) {
		INFO("Missing filename\n");
		return 1;
	}

	if (stat(argv[0], &st) ||
	    (f = fopen(argv[0], "r")) == NULL) {
		INFO("Input file not found\n");
		return 1;
	}

	if (st.st_size <= sizeof(*hdr) + sizeof(*data)) {
		INFO("Input file too small\n");
		fclose(f);
		return 1;
	}

	hdr = calloc(1, st.st_size);
	len = fread(hdr, 1, st.st_size, f);
	fclose(f);

	if (len != st.st_size) {
		INFO("Error reading from input file\n");
		return 1;
	}

	ret = unet_auth_data_validate(pubkey, hdr, len, NULL, NULL);
	switch (ret) {
	case -1:
		INFO("Invalid input data\n");
		break;
	case -2:
		INFO("Public key does not match\n");
		break;
	case -3:
		INFO("Signature verification failed\n");
		break;
	}

	free(hdr);
	return ret;
}

static int cmd_host_pubkey(int argc, char **argv)
{
	size_t pq_len = 0;

	curve25519_generate_public(pubkey, seckey);

	if (seckey_pq_len) {
		if (seckey_pq_len != SNTRUP761_SEC_SIZE) {
			INFO("Post-quantum host key missing\n");
			return 1;
		}

		sntrup761_pubkey(pubkey_pq, seckey_pq);
		pq_len = SNTRUP761_PUB_SIZE;
	} else if (pq_keys || pubkey_pq_len) {
		if (pubkey_pq_len != SNTRUP761_PUB_SIZE) {
			INFO("Invalid post-quantum host key\n");
			return 1;
		}
		pq_len = SNTRUP761_PUB_SIZE;
	}

	print_key(pubkey, pubkey_pq, pq_len);

	return 0;
}

static int cmd_pubkey(int argc, char **argv)
{
	size_t pq_len = 0;

	if (seckey_pq_len) {
		if (seckey_pq_len != MLD_44_SECRETKEYBYTES) {
			INFO("Post-quantum signing key missing/invalid\n");
			return 1;
		}

		MLD_44_ref_pubkey(pubkey_pq, seckey_pq);
		pq_len = MLD_44_PUBLICKEYBYTES;
	} else if (pq_keys || pubkey_pq_len) {
		if (pubkey_pq_len != MLD_44_PUBLICKEYBYTES) {
			INFO("Invalid post-quantum host key\n");
			return 1;
		}
		pq_len = MLD_44_PUBLICKEYBYTES;
	}

	print_key(pubkey, pubkey_pq, pq_len);

	return 0;
}

static int generate_key(void)
{
	if (has_key)
		return 0;

	randombytes(seckey, sizeof(seckey));
	ed25519_prepare(seckey);
	has_key = true;

	return 0;
}

static int cmd_generate_host(int argc, char **argv)
{
	generate_key();

	if (pq_keys && !seckey_pq_len) {
		sntrup761_keypair(pubkey_pq, seckey_pq);
		pubkey_pq_len = SNTRUP761_PUB_SIZE;
		seckey_pq_len = SNTRUP761_SEC_SIZE;
	}

	print_key(seckey, seckey_pq, pq_keys ? SNTRUP761_SEC_SIZE : 0);

	return 0;
}

static int cmd_generate(int argc, char **argv)
{
	generate_key();

	if (pq_keys && !seckey_pq_len) {
		MLD_44_ref_keypair(pubkey_pq, seckey_pq, NULL);
		pubkey_pq_len = MLD_44_PUBLICKEYBYTES;
		seckey_pq_len = MLD_44_SECRETKEYBYTES;
	}

	print_key(seckey, seckey_pq, pq_keys ? MLD_44_SECRETKEYBYTES : 0);

	return 0;
}

static int cmd_netdata(int argc, char **argv)
{
	size_t ofs = sizeof(struct unet_auth_hdr) + sizeof(struct unet_auth_data);

	if (!net_data || net_data_len <= ofs) {
		INFO("Missing network data\n");
		return 1;
	}

	fputs(net_data + ofs, out_file);

	return 0;
}

static bool parse_key(uint8_t *dest, uint8_t *pq_dest, size_t *pq_dest_len, const char *str)
{
	char keystr[B64_ENCODE_LEN(EDSIGN_PUBLIC_KEY_SIZE) + B64_ENCODE_LEN(MAX_KEY_SIZE) + 4];
	char *pq_key;
	FILE *f;
	int len;

	if (!strcmp(str, "-"))
		f = stdin;
	else
		f = fopen(str, "r");

	if (!f) {
		INFO("Can't open key file for reading\n");
		return false;
	}

	len = fread(keystr, 1, sizeof(keystr) - 1, f);
	if (f != stdin)
		fclose(f);

	keystr[len] = 0;

	pq_key = strchr(keystr, ':');
	if (pq_key)
		*(pq_key++) = 0;

	if (b64_decode(keystr, dest, len) != EDSIGN_PUBLIC_KEY_SIZE) {
		INFO("Failed to parse key data\n");
		return false;
	}

	if (pq_key && pq_dest) {
		len = b64_decode(pq_key, pq_dest, MAX_KEY_SIZE);
		if (len < 0) {
			INFO("Failed to parse PQ key data\n");
			return false;
		}

		*pq_dest_len = (size_t)len;
	}

	return true;
}

static void
pbkdf2_hmac_sha512(uint8_t *dest, const void *key, size_t key_len,
		   const void *salt, size_t salt_len,
		   unsigned int rounds)
{
	uint8_t hash[SHA512_HASH_SIZE];

	hmac_sha512(dest, key, key_len, salt, salt_len);

	for (size_t i = 0; i < rounds - 1; i++) {
		hmac_sha512(hash, key, key_len, dest, SHA512_HASH_SIZE);
		for (size_t k = 0; k < SHA512_HASH_SIZE; k++)
			dest[k] ^= hash[k];
	}
}

static bool parse_seed(const char *salt)
{
	uint8_t hash[_max(SHA512_HASH_SIZE, MLDSA_SEEDBYTES)] = {};
	char buf[256], *pw = buf;
	unsigned long rounds;
	size_t len = 0;
	char *sep;

	rounds = strtoul(salt, &sep, 0);
	if (!rounds || *sep != ',') {
		INFO("Invalid number of rounds\n");
		return false;
	}

	if (password_prompt) {
		char *pw2;

		pw = getpass("Password: ");
		if (pw)
			len = strlen(pw);

		if (pw && password_prompt > 1) {
			pw = strcpy(buf, pw);
			pw2 = getpass("Retype password: ");
			if (!pw2 || strcmp(pw, pw2) != 0) {
				INFO("Passwords do not match\n");
				return false;
			}
		}
	} else {
		len = fread(buf, 1, sizeof(buf), stdin);
		if (!feof(stdin)) {
			INFO("Key data too long\n");
			return false;
		}
	}
	if (len < 12) {
		INFO("Key data too short\n");
		return false;
	}

	pbkdf2_hmac_sha512(hash, pw, len, salt, strlen(salt), rounds);
	memcpy(seckey, hash, EDSIGN_PUBLIC_KEY_SIZE);
	has_key = true;

	if (pq_keys) {
		uint8_t pq_salt[SHA512_HASH_SIZE];

		hmac_sha512(pq_salt, seckey, sizeof(seckey), salt, strlen(salt));
		pbkdf2_hmac_sha512(hash, pw, len, pq_salt, sizeof(pq_salt), rounds);

		MLD_44_ref_keypair(pubkey_pq, seckey_pq, hash);
		pubkey_pq_len = MLD_44_PUBLICKEYBYTES;
		seckey_pq_len = MLD_44_SECRETKEYBYTES;
	}

	return true;
}

static bool cmd_needs_peerkey(void)
{
	switch (cmd) {
	case CMD_DOWNLOAD:
		return true;
	default:
		return false;
	}
}

static bool cmd_needs_pubkey(void)
{
	switch (cmd) {
	case CMD_PUBKEY:
	case CMD_DOWNLOAD:
	case CMD_VERIFY:
		return true;
	default:
		return false;
	}
}

static bool cmd_needs_key(void)
{
	switch (cmd) {
	case CMD_SIGN:
	case CMD_HOST_PUBKEY:
		return true;
	default:
		return false;
	}
}

static bool cmd_needs_outfile(void)
{
	switch (cmd) {
	case CMD_SIGN:
	case CMD_PUBKEY:
	case CMD_GENERATE_SIGN:
	case CMD_GENERATE_HOST:
	case CMD_DOWNLOAD:
	case CMD_NETDATA:
		return true;
	default:
		return false;
	}
}

int main(int argc, char **argv)
{
	const char *progname = argv[0];
	const char *out_filename = NULL;
	const char *cmd_arg = NULL;
	bool has_pubkey = false;
	bool has_peerkey = false;
	const char *seed = NULL;
	int ret, ch;

	while ((ch = getopt(argc, argv, "b:h:k:K:o:qQD:gGHpPs:STU:Vx:")) != -1) {
		switch (ch) {
		case 'D':
		case 'U':
		case 'G':
		case 'H':
		case 'S':
		case 'T':
		case 'P':
		case 'V':
			if (cmd != CMD_UNKNOWN)
				return usage(progname);
			break;
		default:
			break;
		}

		switch (ch) {
		case 'q':
			quiet = true;
			break;
		case 'o':
			out_filename = optarg;
			break;
		case 'h':
			if (has_peerkey)
				return usage(progname);

			if (!parse_key(peerkey, NULL, NULL, optarg))
				return 1;

			has_peerkey = true;
			break;
		case 's':
			if (has_pubkey)
				return usage(progname);

			seed = optarg;
			break;
		case 'k':
			if (has_pubkey)
				return usage(progname);

			if (!parse_key(pubkey, pubkey_pq, &pubkey_pq_len, optarg))
				return 1;

			has_pubkey = true;
			break;
		case 'K':
			if (has_pubkey)
				return usage(progname);

			if (!parse_key(seckey, seckey_pq, &seckey_pq_len, optarg))
				return 1;

			has_key = true;
			break;
		case 'p':
			password_prompt++;
			break;
		case 'b':
			if (load_network_data(optarg))
				return 1;

			has_pubkey = true;
			break;
		case 'x':
			if (!parse_key(xorkey, xorkey_pq, &xorkey_pq_len, optarg))
				return 1;

			has_xor = true;
			break;
		case 'Q':
			pq_keys = true;
			break;
		case 'U':
			cmd = CMD_UPLOAD;
			cmd_arg = optarg;
			break;
		case 'D':
			cmd = CMD_DOWNLOAD;
			cmd_arg = optarg;
			break;
		case 'g':
			cmd = CMD_GENERATE_HOST;
			break;
		case 'G':
			cmd = CMD_GENERATE_SIGN;
			break;
		case 'S':
			cmd = CMD_SIGN;
			break;
		case 'T':
			cmd = CMD_NETDATA;
			break;
		case 'P':
			cmd = CMD_PUBKEY;
			break;
		case 'H':
			cmd = CMD_HOST_PUBKEY;
			break;
		case 'V':
			cmd = CMD_VERIFY;
			break;
		default:
			return usage(progname);
		}
	}

	if (seed && !parse_seed(seed))
		return 1;

	if (has_key) {
		if (has_xor)
			for (size_t i = 0; i < ARRAY_SIZE(seckey); i++)
				seckey[i] ^= xorkey[i];

		if (xorkey_pq_len) {
			if (seckey_pq_len != xorkey_pq_len) {
				INFO("PQ xor key length mismatch: %d != %d\n", (int)seckey_pq_len, (int)xorkey_pq_len);
				return 1;
			}

			for (size_t i = 0; i < ARRAY_SIZE(seckey_pq); i++)
				seckey_pq[i] ^= xorkey_pq[i];
		}

		edsign_sec_to_pub(pubkey, seckey);
		has_pubkey = true;
	}

	if (!has_peerkey && cmd_needs_peerkey()) {
		INFO("Missing -h <key> argument\n");
		return 1;
	}

	if (!has_key && cmd_needs_key()) {
		INFO("Missing -K <key> argument\n");
		return 1;
	}

	if (!has_pubkey && cmd_needs_pubkey()) {
		INFO("Missing -k <key> argument\n");
		return 1;
	}

	argc -= optind;
	argv += optind;

	if (out_filename && cmd_needs_outfile()) {
		out_file = fopen(out_filename, "w");
		if (!out_file) {
			INFO("Failed to open output file\n");
			return 1;
		}
	} else {
		out_file = stdout;
	}

	ret = -1;
	switch (cmd) {
	case CMD_UPLOAD:
	case CMD_DOWNLOAD:
		ret = cmd_sync(cmd_arg, argc, argv);
		break;
	case CMD_GENERATE_HOST:
		ret = cmd_generate_host(argc, argv);
		break;
	case CMD_GENERATE_SIGN:
		ret = cmd_generate(argc, argv);
		break;
	case CMD_SIGN:
		ret = cmd_sign(argc, argv);
		break;
	case CMD_PUBKEY:
		ret = cmd_pubkey(argc, argv);
		break;
	case CMD_HOST_PUBKEY:
		ret = cmd_host_pubkey(argc, argv);
		break;
	case CMD_VERIFY:
		ret = cmd_verify(argc, argv);
		break;
	case CMD_NETDATA:
		ret = cmd_netdata(argc, argv);
		break;
	case CMD_UNKNOWN:
		ret = usage(progname);
		break;
	}

	if (net_data)
		free(net_data);

	blob_buf_free(&b);

	if (out_file != stdout) {
		fclose(out_file);
		if (ret)
			unlink(out_filename);
	}

	return ret;
}

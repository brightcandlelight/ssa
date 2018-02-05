#include <linux/hashtable.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/ctype.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/uaccess.h>
#include "socktls.h"
#include "tls_common.h"
#include "netlink.h"

#define HASH_TABLE_BITSIZE	9
#define MAX_HOST_LEN		255

/* Helpers */
int get_hostname(struct socket* sock, char __user *optval, int* __user len);
int get_id(struct socket* sock, char __user *optval, int* __user optlen);
int set_hostname(tls_sock_data_t* sock_data, char* optval, unsigned int len);
static int is_valid_host_string(char* str, int len);

static DEFINE_HASHTABLE(tls_sock_data_table, HASH_TABLE_BITSIZE);
static DEFINE_SPINLOCK(tls_sock_data_table_lock);

/**
 * Finds a socket option in the hash table
 * @param	key - A pointer to the sock struct related to the socket option
 * @return	TLS socket data associated with key, if any. If not found, returns NULL
 */
tls_sock_data_t* get_tls_sock_data(unsigned long key) {
	tls_sock_data_t* it;
	hash_for_each_possible(tls_sock_data_table, it, hash, (unsigned long)key) {
		if (it->key == key) {
			return it;
		}
	}
	return NULL;
}

void put_tls_sock_data(unsigned long key, struct hlist_node* hash) {
	spin_lock(&tls_sock_data_table_lock);
	hash_add(tls_sock_data_table, hash, key);
	spin_unlock(&tls_sock_data_table_lock);
	return;
}

void rem_tls_sock_data(struct hlist_node* hash) {
	spin_lock(&tls_sock_data_table_lock);
	hash_del(hash);
	spin_unlock(&tls_sock_data_table_lock);
	return;
}

void tls_setup(void) {
	register_netlink();
	hash_init(tls_sock_data_table);
	return;
}

void tls_cleanup(void) {
        int bkt;
        tls_sock_data_t* it;
        struct hlist_node tmp;
        struct hlist_node* tmpptr = &tmp;

        spin_lock(&tls_sock_data_table_lock);
        hash_for_each_safe(tls_sock_data_table, bkt, tmpptr, it, hash) {
		/*if (it->int_addr.sa_family == AF_INET) {
			(*ref_tcp_v4_destroy_sock)(it->sk);
		}
		else {
			(*ref_unix_release)((it->sk)->sk_socket);
		}*/
                hash_del(&it->hash);
                kfree(it->hostname);
		kfree(it);
        }
        spin_unlock(&tls_sock_data_table_lock);

	unregister_netlink();

	return;
}


void report_return(unsigned long key, int ret) {
	tls_sock_data_t* sock_data;
	sock_data = get_tls_sock_data(key);
	//BUG_ON(sock_data == NULL);
	if (sock_data == NULL) {
		return;
	}
	sock_data->response = ret;
	complete(&sock_data->sock_event);
	return;
}

void report_data_return(unsigned long key, char* data, unsigned int len) {
	tls_sock_data_t* sock_data;
	sock_data = get_tls_sock_data(key);
	//BUG_ON(sock_data == NULL);
	if (sock_data == NULL) {
		return;
	}
	sock_data->rdata = kmalloc(len, GFP_KERNEL);
	if (sock_data->rdata == NULL) {
		printk(KERN_ALERT "Failed to create memory for getsockopt return\n");
	}
	memcpy(sock_data->rdata, data, len);
	sock_data->rdata_len = len;
	/* set success if this callback is used.
	 * The report_return case is for errors
	 * and simple statuses */
	sock_data->response = 0;
	complete(&sock_data->sock_event);
	return;
}

int tls_common_setsockopt(tls_sock_data_t* sock_data, struct socket *sock, int level, int optname, char __user *optval, unsigned int optlen, setsockopt_t orig_func) {
	int ret;
	char* koptval;
	if (optval == NULL) {
		return -EINVAL;	
	}
	if (optlen == 0) {
		return -EINVAL;
	}
	koptval = kmalloc(optlen, GFP_KERNEL);
	if (koptval == NULL) {
		return -ENOMEM;
	}
	if (copy_from_user(koptval, optval, optlen) != 0) {
		kfree(koptval);
		return -EFAULT;
	}

	/* Here we save all TLS-specific sockopt values so that
	 * we can retrieve them directly from the kernel when
	 * the application uses getsockopt */
	switch (optname) {
		case SO_HOSTNAME:
			ret = set_hostname(sock_data, koptval, optlen);
			break;
		case SO_CERTIFICATE_CHAIN:
		case SO_PRIVATE_KEY:
		default:
			ret = 0;
			break;
	}

	/* We return early if preliminary checks during our
	 * kernel-side saving of sockopts failed. No sense
	 * in telling the daemon about it. */
	if (ret != 0) {
		kfree(koptval);
		return ret;
	}

	send_setsockopt_notification((unsigned long)sock, level, optname, koptval, optlen, sock_data->daemon_id);
	kfree(koptval);
	if (wait_for_completion_timeout(&sock_data->sock_event, RESPONSE_TIMEOUT) == 0) {
		/* Let's lie to the application if the daemon isn't responding */
		return -ENOBUFS;
	}
	if (sock_data->response != 0) {
		return sock_data->response;
	}

	/* We only get here if the daemonside setsockopt succeeded */

	switch (optname) {
		case SO_HOSTNAME:
		case SO_CERTIFICATE_CHAIN:
		case SO_PRIVATE_KEY:
			break;
		default:
			/* Now we do the same thing to the application socket, if applicable */
			if (orig_func != NULL) {
				return orig_func(sock, level, optname, optval, optlen);
			}
			return -EOPNOTSUPP;
			break;
	}
	return 0;
}


int tls_common_getsockopt(tls_sock_data_t* sock_data, struct socket *sock, int level, int optname, char __user *optval, int __user *optlen, getsockopt_t orig_func) {
	int len;
	if (get_user(len, optlen)) {
		return -EFAULT;
	}
	switch (optname) {
		case SO_HOSTNAME:
			return get_hostname(sock, optval, optlen);
		case SO_ID:
			return get_id(sock, optval, optlen);
		case SO_PEER_CERTIFICATE:
		/* We'll probably add all other daemon-required getsockopt options here
		 * as fall-through cases. The following implementation is fairly generic.
		 */
			send_getsockopt_notification((unsigned long)sock, level, optname, sock_data->daemon_id);
			if (wait_for_completion_timeout(&sock_data->sock_event, RESPONSE_TIMEOUT) == 0) {
				/* Let's lie to the application if the daemon isn't responding */
				return -ENOBUFS;
			}
			if (sock_data->response != 0) {
				return sock_data->response;
			}


			/* We set this to the minimum of actual data length and size
			 * of user's buffer rather than aborting if the user one is 
			 * smaller because POSIX says to silently truncate in this
			 * case */
			len = min_t(unsigned int, len, sock_data->rdata_len);
			if (unlikely(put_user(len, optlen))) {
				kfree(sock_data->rdata);
				sock_data->rdata = NULL;
				sock_data->rdata_len = 0;
				return -EFAULT;
			}
			if (copy_to_user(optval, sock_data->rdata, len)) {
				kfree(sock_data->rdata);
				sock_data->rdata = NULL;
				sock_data->rdata_len = 0;
				return -EFAULT;
			}
			break;
		default:
			if (orig_func != NULL) {
				return orig_func(sock, level, optname, optval, optlen);
			}
			return -EOPNOTSUPP;
	}
	return 0;
}

int set_hostname(tls_sock_data_t* sock_data, char* optval, unsigned int len) {
	if (sock_data->is_connected == 1) {
		return -EISCONN;
	}
	if (len > MAX_HOST_LEN) {
		return -EINVAL;
	}
	sock_data->hostname = krealloc(sock_data->hostname, len, GFP_KERNEL);
	if (sock_data->hostname == NULL) {
		return -ENOMEM;
	}
	if (!is_valid_host_string(optval, len)) {
		return -EINVAL;
	}
	memcpy(sock_data->hostname, optval, len);
	return  0;
}

int get_hostname(struct socket* sock, char __user *optval, int* __user len) {
	int hostname_len;
	tls_sock_data_t* data;
	char* hostname = NULL;
	if ((data = get_tls_sock_data((unsigned long)sock)) == NULL) {
		return -EBADF;
	}
	hostname = data->hostname;
	if (hostname == NULL) {
		return -EFAULT;
	}
	hostname_len = strnlen(hostname, MAX_HOST_LEN) + 1;
	if (*len < hostname_len) {
		return -EINVAL;	
	}
	if (copy_to_user(optval, hostname, hostname_len) != 0 ) {
		return -EFAULT;
	}
	*len = hostname_len;
	return 0;
}

/* The ID is just the pointer value sk */
int get_id(struct socket* sock, char __user *optval, int* __user optlen) {
	int len;
	if (get_user(len, optlen)) {
		return -EFAULT;
	}
	len = min_t(unsigned int, len, sizeof(sock));
	if (put_user(len, optlen)) {
		return -EFAULT;
	}
	if (copy_to_user(optval, &sock, len)) {
		return -EFAULT;
	}
	return 0;
}

/* 
 * Tests whether a socket option input contains only valid host name characters
 * as defined by RFC 952 and RFC 1123.
 * @param	str - A pointer to a string to be checked
 * @param	len - The length of str, including null terminator 
 * @return	1 if string is valid and 0 otherwise
 */
int is_valid_host_string(char* str, int len) {
	int i;
	char c;
	for (i = 0; i < len-1; i++) {
		c = str[i];
                if (!isalnum(c) && c != '-' && c != '.') {
			return 0;
                }
        }
	if (str[len-1] != '\0') {
		return 0;
	}
        return 1;
}

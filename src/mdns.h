#ifndef VITADROP_MDNS_H
#define VITADROP_MDNS_H

/*
 * Answers "where is vitadrop.local?" questions on the Wi-Fi network (mDNS),
 * so phones and computers can reach the Vita by name instead of an IP address.
 */
#define MDNS_HOSTNAME "vitadrop.local"

int mdns_start(void);
void mdns_stop(void);

/* Tell it the Vita's current IP address, e.g. "192.168.1.25". */
void mdns_set_ip(const char *ip);

/* 1 if the name is being answered right now. */
int mdns_is_active(void);

#endif

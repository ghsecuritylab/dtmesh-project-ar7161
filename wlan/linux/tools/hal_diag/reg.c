/*
 * Copyright (c) 2009, Atheros Communications Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * $Id: //depot/sw/releases/7.3_AP/wlan/linux/tools/hal_diag/reg.c#3 $
 */
#include "diag.h"

#include "ah.h"
#include "ah_internal.h"

#include <getopt.h>
#include <errno.h>
#include <err.h>

struct	ath_diag atd;
int	s;
const char *progname;

u_int32_t
regread(u_int32_t off)
{
	u_int32_t regdata;

	atd.ad_id = HAL_DIAG_REGREAD | ATH_DIAG_IN | ATH_DIAG_DYN;
	atd.ad_in_size = sizeof(off);
	atd.ad_in_data = (caddr_t) &off;
	atd.ad_out_size = sizeof(regdata);
	atd.ad_out_data = (caddr_t) &regdata;
	if (ioctl(s, SIOCGATHDIAG, &atd) < 0)
		err(1, atd.ad_name);

	return regdata;
}

HAL_BUS_HANDLE
getregbase()
{
	HAL_BUS_HANDLE base;

	atd.ad_id = HAL_DIAG_GET_REGBASE | ATH_DIAG_DYN;
	atd.ad_out_size = sizeof(base);
	atd.ad_out_data = (caddr_t) &base;
	if (ioctl(s, SIOCGATHDIAG, &atd) < 0)
		err(1, atd.ad_name);
	return base;
}

void
getdmadbg()
{
#if 0
	atd.ad_id = HAL_DIAG_DMADBG | ATH_DIAG_IN;
	if (ioctl(s, SIOCGATHDIAG, &atd) < 0)
		err(1, atd.ad_name);
#endif
}

static void
regwrite(u_long off, u_long value)
{
	HAL_DIAG_REGVAL regval;

	regval.offset = (u_int) off ;
	regval.val = (u_int32_t) value;

	atd.ad_id = HAL_DIAG_REGWRITE | ATH_DIAG_IN;
	atd.ad_in_size = sizeof(regval);
	atd.ad_in_data = (caddr_t) &regval;
	atd.ad_out_size = 0;
	atd.ad_out_data = NULL;
	if (ioctl(s, SIOCGATHDIAG, &atd) < 0)
		err(1, atd.ad_name);
}

static void
usage()
{
	fprintf(stderr, "usage: %s [-i] [-d] [offset [# registers to read] | offset=value]\n", progname);
	exit(-1);
}

int
main(int argc, char *argv[])
{
	HAL_REVS revs;
	const char *ifname = ATH_DEFAULT;
	int c;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		err(1, "socket");

	progname = argv[0];
	while ((c = getopt(argc, argv, "di:")) != -1)
		switch (c) {
		case 'i':
			ifname = optarg;
			break;
		case 'd':
			strncpy(atd.ad_name, ifname, sizeof (atd.ad_name));
			getdmadbg();
			return 0;
		default:
			usage();
			/*NOTREACHED*/
		}
	argc -= optind;
	argv += optind;

	strncpy(atd.ad_name, ifname, sizeof (atd.ad_name));

	for (; argc > 0; argc--, argv++) {
		u_int   off, cnt, i;
		u_int32_t  val, oval,regbase;
		char line[256];
		char *cp;
		cp = strchr(argv[0], '=');
		if (cp != NULL)
			*cp = '\0';
		off = (u_int) strtoul(argv[0], 0, NULL);
		if (off == 0 && errno == EINVAL)
			errx(1, "%s: invalid reg offset %s",
			     progname, argv[0]);
		if (cp == NULL) {
		    if (argv[1])
		        cnt = (u_int) strtoul(argv[1], 0, NULL);
		    else
		        cnt = 1;
		    for (i = 0; i < cnt; i++) {
		        printf("0x%04x = 0x%08x\n", off+(4*i), regread(off+(4*i)));
		    }
		    return 0;
		} else {
			val = (u_int32_t) strtoul(cp+1, 0, NULL);
			if (val == 0 && errno == EINVAL)
				errx(1, "%s: invalid reg value %s",
				     progname, cp+1);
			oval = regread(off);
			printf("Write %04x: %04x = %04x\n",
			       off, oval, val);
			regwrite(off, val);
		}
	}
	return 0;
}


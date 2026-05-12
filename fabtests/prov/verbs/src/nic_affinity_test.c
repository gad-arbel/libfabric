/*
 * Copyright (c) Intel Corporation.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *	- Redistributions of source code must retain the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer.
 *
 *	- Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials
 *	  provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>

#include <rdma/fi_errno.h>

#include "shared.h"
#include "unit_common.h"

#define OFI_CORE_PROV_ONLY      (1ULL << 59)
#define TEST_PCI_ADDR           "0000:00:00.0"
#define TEST_CONFIG_FILE        "/tmp/test_config.conf"

#define TEST_ENTRY_NIC_AFFINITY(name) \
	TEST_ENTRY(nic_affinity_##name, nic_affinity_##name##_desc)

typedef int (*ft_nic_affinity_init)(struct fi_info *);
typedef int (*ft_nic_affinity_test)(struct fi_info *);

static char err_buf[512];

static const char *get_nic_name(struct fi_info *info)
{
	if (info->nic && info->nic->device_attr && info->nic->device_attr->name)
		return info->nic->device_attr->name;
	return NULL;
}

static int get_arbitrary_nic_name(char *nic_name, size_t len)
{
	struct fi_info *info = NULL;
	int ret;

	ret = fi_getinfo(FT_FIVERSION, NULL, NULL, 0, hints, &info);
	if (ret) {
		sprintf(err_buf, "fi_getinfo failed to discover NICs: %s", fi_strerror(-ret));
		return ret;
	}

	if (!info) {
		sprintf(err_buf, "No provider info returned");
		return -FI_ENODATA;
	}

	for (struct fi_info *cur = info; cur; cur = cur->next) {
		const char *name = get_nic_name(cur);
		if (name) {
			snprintf(nic_name, len, "%s", name);
			fi_freeinfo(info);
			return 0;
		}
	}

	fi_freeinfo(info);
	sprintf(err_buf, "No NIC names found in provider info");
	return -FI_ENODATA;
}

static int create_valid_test_config_file(void)
{
	FILE *fp;
	char nic_name[64];
	int ret;

	ret = get_arbitrary_nic_name(nic_name, sizeof(nic_name));
	if (ret) return ret;

	fp = fopen(TEST_CONFIG_FILE, "w");
	if (!fp) {
		sprintf(err_buf, "Failed to open config file for writing");
		return -FI_EIO;
	}

	fprintf(fp, "%s %s\n", TEST_PCI_ADDR, nic_name);
	fclose(fp);

	return 0;
}

static int create_invalid_test_config_file(void)
{
	FILE *fp;

	fp = fopen(TEST_CONFIG_FILE, "w");
	if (!fp) {
		sprintf(err_buf, "Failed to open config file for writing");
		return -FI_EIO;
	}

	fprintf(fp, "invalid_config_line\n");
	fclose(fp);

	return 0;
}

static void cleanup_nic_affinity_test(void)
{
	unlink(TEST_CONFIG_FILE);
	unsetenv("FI_VERBS_NIC_AFFINITY_POLICY");
	unsetenv("FI_VERBS_AFFINITY_DEVICE");
	unsetenv("FI_VERBS_NIC_AFFINITY_CONFIG");
}

/*
 * Init functions
 */
static int init_manual(struct fi_info *hints)
{
	int ret;

	ret = create_valid_test_config_file();
	if (ret) return ret;

	setenv("FI_VERBS_NIC_AFFINITY_POLICY", "manual", 1);
	setenv("FI_VERBS_NIC_AFFINITY_CONFIG", TEST_CONFIG_FILE, 1);
	return 0;
}

static int init_manual_no_device(struct fi_info *hints)
{
	int ret;

	ret = create_valid_test_config_file();
	if (ret) return ret;

	setenv("FI_VERBS_NIC_AFFINITY_POLICY", "manual", 1);
	setenv("FI_VERBS_NIC_AFFINITY_CONFIG", TEST_CONFIG_FILE, 1);
	unsetenv("FI_VERBS_AFFINITY_DEVICE");
	return 0;
}

static int init_manual_invalid_device(struct fi_info *hints)
{
	int ret;

	ret = create_valid_test_config_file();
	if (ret) return ret;

	setenv("FI_VERBS_NIC_AFFINITY_POLICY", "manual", 1);
	setenv("FI_VERBS_NIC_AFFINITY_CONFIG", TEST_CONFIG_FILE, 1);
	setenv("FI_VERBS_AFFINITY_DEVICE", "invalid:pci:format:bad", 1);
	return 0;
}

static int init_manual_missing_config(struct fi_info *hints)
{
	setenv("FI_VERBS_NIC_AFFINITY_POLICY", "manual", 1);
	setenv("FI_VERBS_NIC_AFFINITY_CONFIG", "/nonexistent/path/to/config.conf", 1);
	return 0;
}

static int init_manual_malformed_config(struct fi_info *hints)
{
	int ret;

	ret = create_invalid_test_config_file();
	if (ret) return ret;

	setenv("FI_VERBS_NIC_AFFINITY_POLICY", "manual", 1);
	setenv("FI_VERBS_NIC_AFFINITY_CONFIG", TEST_CONFIG_FILE, 1);
	return 0;
}

static int init_auto(struct fi_info *hints)
{
	setenv("FI_VERBS_NIC_AFFINITY_POLICY", "auto", 1);
	return 0;
}

static int init_auto_no_device(struct fi_info *hints)
{
	setenv("FI_VERBS_NIC_AFFINITY_POLICY", "auto", 1);
	unsetenv("FI_VERBS_AFFINITY_DEVICE");
	return 0;
}

static int init_auto_invalid_device(struct fi_info *hints)
{
	setenv("FI_VERBS_NIC_AFFINITY_POLICY", "auto", 1);
	setenv("FI_VERBS_AFFINITY_DEVICE", "invalid:pci:format:bad", 1);
	return 0;
}

static int init_invalid(struct fi_info *hints)
{
	setenv("FI_VERBS_NIC_AFFINITY_POLICY", "invalid_garbage_policy", 1);
	return 0;
}

/*
 * Check functions
 */
static int check_count_and_grouping(struct fi_info *original_info, struct fi_info *policy_info)
{
	struct fi_info *original_cur;
	struct fi_info *affinity_cur;
	const char *nic_to_find;
	size_t original_count;
	size_t policy_count;

	original_cur = original_info;
	while (original_cur) {
		nic_to_find = get_nic_name(original_cur);
		if (!nic_to_find) {
			original_cur = original_cur->next;
			continue;
		}

		original_count = 0;
		while (original_cur && get_nic_name(original_cur) &&
		       strcmp(get_nic_name(original_cur), nic_to_find) == 0) {
			original_count++;
			original_cur = original_cur->next;
		}

		for (affinity_cur = policy_info; affinity_cur; affinity_cur = affinity_cur->next) {
			if (get_nic_name(affinity_cur) &&
			    strcmp(get_nic_name(affinity_cur), nic_to_find) == 0)
				break;
		}

		policy_count = 0;
		while (affinity_cur && get_nic_name(affinity_cur) &&
		       strcmp(get_nic_name(affinity_cur), nic_to_find) == 0) {
			policy_count++;
			affinity_cur = affinity_cur->next;
		}

		if (original_count != policy_count) {
			sprintf(err_buf, "NIC %s: original has %zu entries, policy has %zu consecutive entries",
				nic_to_find, original_count, policy_count);
			return EXIT_FAILURE;
		}
	}

	return 0;
}

static int compare_lists_same_order(struct fi_info *list1, struct fi_info *list2)
{
	struct fi_info *cur1;
	struct fi_info *cur2;
	const char *name1;
	const char *name2;

	cur1 = list1;
	cur2 = list2;
	while (cur1 && cur2) {
		name1 = get_nic_name(cur1);
		name2 = get_nic_name(cur2);

		if (name1 && name2 && strcmp(name1, name2) != 0) {
			sprintf(err_buf, "Order mismatch: %s != %s", name1, name2);
			return EXIT_FAILURE;
		}

		cur1 = cur1->next;
		cur2 = cur2->next;
	}

	if (cur1 || cur2) {
		sprintf(err_buf, "Different number of entries");
		return EXIT_FAILURE;
	}

	return 0;
}

static int check_no_interference(struct fi_info *hints)
{
	struct fi_info *original_info = NULL;
	struct fi_info *policy_info1 = NULL;
	struct fi_info *policy_info2 = NULL;
	int ret;

	ret = fi_getinfo(FT_FIVERSION, NULL, NULL, OFI_CORE_PROV_ONLY, hints, &policy_info1);
	if (ret) {
		FT_UNIT_STRERR(err_buf, "fi_getinfo with affinity policy failed", ret);
		return ret;
	}

	ret = fi_getinfo(FT_FIVERSION, NULL, NULL, OFI_CORE_PROV_ONLY, hints, &policy_info2);
	if (ret) {
		FT_UNIT_STRERR(err_buf, "fi_getinfo with affinity policy (second call) failed", ret);
		fi_freeinfo(policy_info1);
		return ret;
	}

	ret = compare_lists_same_order(policy_info1, policy_info2);
	if (ret)
		goto cleanup;

	unsetenv("FI_VERBS_NIC_AFFINITY_POLICY");
	unsetenv("FI_VERBS_AFFINITY_DEVICE");

	ret = fi_getinfo(FT_FIVERSION, NULL, NULL, OFI_CORE_PROV_ONLY, hints, &original_info);
	if (ret) {
		FT_UNIT_STRERR(err_buf, "fi_getinfo with policy=none failed", ret);
		goto cleanup;
	}

	ret = check_count_and_grouping(original_info, policy_info1);

cleanup:
	fi_freeinfo(original_info);
	fi_freeinfo(policy_info1);
	fi_freeinfo(policy_info2);

	cleanup_nic_affinity_test();

	return ret;
}

static int check_identical_list(struct fi_info *hints)
{
	struct fi_info *original_info = NULL;
	struct fi_info *policy_info = NULL;
	int ret;

	ret = fi_getinfo(FT_FIVERSION, NULL, NULL, OFI_CORE_PROV_ONLY, hints, &policy_info);
	if (ret) {
		FT_UNIT_STRERR(err_buf, "fi_getinfo with affinity policy failed", ret);
		cleanup_nic_affinity_test();
		return ret;
	}

	unsetenv("FI_VERBS_NIC_AFFINITY_POLICY");
	unsetenv("FI_VERBS_AFFINITY_DEVICE");

	ret = fi_getinfo(FT_FIVERSION, NULL, NULL, OFI_CORE_PROV_ONLY, hints, &original_info);
	if (ret) {
		FT_UNIT_STRERR(err_buf, "fi_getinfo with policy=none failed", ret);
		fi_freeinfo(policy_info);
		cleanup_nic_affinity_test();
		return ret;
	}

	ret = compare_lists_same_order(original_info, policy_info);

	fi_freeinfo(original_info);
	fi_freeinfo(policy_info);

	cleanup_nic_affinity_test();

	return ret;
}

/*
 * nic affinity test
 */
static int nic_affinity_unit_test(ft_nic_affinity_init init,
				   ft_nic_affinity_test test)
{
	struct fi_info *info = NULL, *test_hints = NULL;
	int ret;

	test_hints = fi_dupinfo(hints);
	if (!test_hints)
		return -FI_ENOMEM;

	if (init) {
		ret = init(test_hints);
		if (ret)
			goto out;
	}

	if (test) {
		ret = test(test_hints);
	} else {
		ret = fi_getinfo(FT_FIVERSION, NULL, NULL, 0,
				 test_hints, &info);
	}
	if (ret) {
		sprintf(err_buf, "fi_getinfo returned %d - %s",
			-ret, fi_strerror(-ret));
		goto out;
	}

out:
	fi_freeinfo(test_hints);
	fi_freeinfo(info);
	return ret;
}

#define nic_affinity_test(name, desc, init, test)			\
char *nic_affinity_ ## name ## _desc = desc;				\
static int nic_affinity_ ## name(void)					\
{									\
	int ret, testret = FAIL;					\
	ret = nic_affinity_unit_test(init, test);			\
	if (ret)							\
		goto fail;						\
	testret = PASS;							\
fail:									\
	return TEST_RET_VAL(ret, testret);				\
}

/*
 * Tests:
 */
nic_affinity_test(manual_sanity, "Test manual policy for sanity",
		  init_manual,
		  check_no_interference)
nic_affinity_test(manual_no_device, "Test manual policy without device",
		  init_manual_no_device,
		  check_identical_list)
nic_affinity_test(manual_invalid_device, "Test manual policy with invalid device",
		  init_manual_invalid_device,
		  check_identical_list)
nic_affinity_test(manual_missing_config, "Test manual policy with missing config file",
		  init_manual_missing_config,
		  check_identical_list)
nic_affinity_test(manual_malformed_config, "Test manual policy with malformed config",
		  init_manual_malformed_config,
		  check_identical_list)
nic_affinity_test(auto_sanity, "Test auto policy for sanity",
		  init_auto,
		  check_no_interference)
nic_affinity_test(auto_no_device, "Test auto policy without device",
		  init_auto_no_device,
		  check_identical_list)
nic_affinity_test(auto_invalid_device, "Test auto policy with invalid device",
		  init_auto_invalid_device,
		  check_identical_list)
nic_affinity_test(invalid_policy, "Test invalid policy fallback to none",
		  init_invalid,
		  check_identical_list)

static void usage(char *name)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "  %s [-h]\n\n", name);
	fprintf(stderr, "Unit tests for verbs GPU-NIC affinity feature\n\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -h    display this help output\n");
}

int main(int argc, char **argv)
{
	int failed, cleanup_ret;
	int op;

	struct test_entry nic_affinity_tests[] = {
		TEST_ENTRY_NIC_AFFINITY(manual_sanity),
		TEST_ENTRY_NIC_AFFINITY(manual_no_device),
		TEST_ENTRY_NIC_AFFINITY(manual_invalid_device),
		TEST_ENTRY_NIC_AFFINITY(manual_missing_config),
		TEST_ENTRY_NIC_AFFINITY(manual_malformed_config),
		TEST_ENTRY_NIC_AFFINITY(auto_sanity),
		TEST_ENTRY_NIC_AFFINITY(auto_no_device),
		TEST_ENTRY_NIC_AFFINITY(auto_invalid_device),
		TEST_ENTRY_NIC_AFFINITY(invalid_policy),
		{ NULL, "" }
	};

	opts = INIT_OPTS;

	hints = fi_allocinfo();
	if (!hints)
		return EXIT_FAILURE;

	while ((op = getopt(argc, argv, "h")) != -1) {
		switch (op) {
		case '?':
		case 'h':
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	hints->fabric_attr->prov_name = strdup("verbs");
	if (!hints->fabric_attr->prov_name) {
		fi_freeinfo(hints);
		return EXIT_FAILURE;
	}

	hints->mode = ~0;

	setenv("FI_VERBS_AFFINITY_DEVICE", TEST_PCI_ADDR, 1);
	failed = run_tests(nic_affinity_tests, err_buf);
	unsetenv("FI_VERBS_AFFINITY_DEVICE");

	if (failed > 0) {
		printf("\nSummary: %d tests failed\n", failed);
	} else {
		printf("\nSummary: all tests passed\n");
	}

	cleanup_ret = ft_free_res();
	return cleanup_ret ? ft_exit_code(cleanup_ret) :
		(failed > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}

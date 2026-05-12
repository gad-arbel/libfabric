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
#include <sys/wait.h>
#include <unistd.h>

#include <rdma/fi_errno.h>

#include "shared.h"
#include "unit_common.h"

#define OFI_CORE_PROV_ONLY      (1ULL << 59)
#define MAX_NICS                64
#define MAX_NIC_NAME_LEN        64
#define MAX_NIC_LIST_SIZE       (MAX_NICS * (MAX_NIC_NAME_LEN + 1) + 16)
#define TEST_PCI_ADDR           "0000:00:00.0"
#define TEST_CONFIG_FILE        "/tmp/test_config.conf"

#define TEST_ENTRY_NIC_AFFINITY(name) \
	TEST_ENTRY(nic_affinity_##name, nic_affinity_##name##_desc)

typedef int (*ft_nic_affinity_init)(struct fi_info *);
typedef int (*ft_nic_affinity_test)(char current_nics[][MAX_NIC_NAME_LEN]);

static char err_buf[512];
static int baseline_total_count;
static int baseline_count;
static char baseline_nics[MAX_NICS][MAX_NIC_NAME_LEN];

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

static int serialize_nic_list(struct fi_info *info)
{
	char buffer[MAX_NIC_LIST_SIZE];
	struct fi_info *cur;
	const char *name, *prev_name = NULL;
	size_t offset = 0;
        int total_count = 0;
	int unique_count = 0;
	int ret;

	for (cur = info; cur; cur = cur->next) {
                total_count++;

		name = get_nic_name(cur);
		if (!name)
			continue;

		if (prev_name && strcmp(name, prev_name) == 0)
			continue;

		if (strlen(name) >= MAX_NIC_NAME_LEN) {
			sprintf(err_buf, "NIC name too long: %s (max %d)", name,
				MAX_NIC_NAME_LEN - 1);
			return -FI_ENOMEM;
		}

		ret = snprintf(buffer + offset, sizeof(buffer) - offset, "%s,", name);
		if (ret < 0 || (size_t)ret >= sizeof(buffer) - offset) {
			sprintf(err_buf, "Buffer overflow serializing NIC list");
			return -FI_ENOMEM;
		}
		offset += ret;
		unique_count++;
		prev_name = name;
	}

	if (offset > 0)
		buffer[offset - 1] = '\0';

	printf("%d:%d:%s", total_count, unique_count, buffer);
	fflush(stdout);
	return 0;
}

static int deserialize_nic_list(const char *serialized, char nics[][MAX_NIC_NAME_LEN],
				int *total_count, int *unique_count)
{
	char buffer[MAX_NIC_LIST_SIZE];
	char *token, *saveptr;
	char *first_colon, *second_colon;
	int i = 0;

	first_colon = strchr(serialized, ':');
	if (!first_colon) {
		sprintf(err_buf, "Invalid format in serialized output");
		return -FI_EINVAL;
	}

	second_colon = strchr(first_colon + 1, ':');
	if (!second_colon) {
		sprintf(err_buf, "Invalid format in serialized output (missing second colon)");
		return -FI_EINVAL;
	}

	*total_count = atoi(serialized);
	*unique_count = atoi(first_colon + 1);

	strncpy(buffer, second_colon + 1, sizeof(buffer) - 1);
	buffer[sizeof(buffer) - 1] = '\0';

	token = strtok_r(buffer, ",", &saveptr);
	while (token && i < MAX_NICS) {
		strncpy(nics[i], token, MAX_NIC_NAME_LEN);
		i++;
		token = strtok_r(NULL, ",", &saveptr);
	}

	if (i != *unique_count) {
		sprintf(err_buf, "Unique count mismatch: expected %d, got %d", *unique_count, i);
		return -FI_EINVAL;
	}

	return 0;
}

/*
 * Init functions
 */
static int init_none(struct fi_info *hints)
{
	setenv("FI_VERBS_NIC_AFFINITY_POLICY", "none", 1);
	return 0;
}

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
 * Check functions - run in parent process after receiving serialized NIC lists
 */
static int check_same_nics(char policy_nics[][MAX_NIC_NAME_LEN])
{
	int i, j;
	int found;

	for (i = 0; i < baseline_count; i++) {
		found = 0;
		for (j = 0; j < baseline_count; j++) {
			if (strcmp(baseline_nics[i], policy_nics[j]) == 0) {
				found = 1;
				break;
			}
		}
		if (!found) {
			sprintf(err_buf, "NIC %s from baseline not found in policy list",
				baseline_nics[i]);
			return EXIT_FAILURE;
		}
	}

	return 0;
}

static int check_identical_list(char current_nics[][MAX_NIC_NAME_LEN])
{
	int i;

	for (i = 0; i < baseline_count; i++) {
		if (strcmp(baseline_nics[i], current_nics[i]) != 0) {
			sprintf(err_buf, "NIC list mismatch at index %d: baseline=%s, current=%s",
				i, baseline_nics[i], current_nics[i]);
			return EXIT_FAILURE;
		}
	}

	return 0;
}

static void getinfo_wrapper(void)
{
	struct fi_info *info = NULL;
	int ret;

	ret = fi_getinfo(FT_FIVERSION, NULL, NULL, OFI_CORE_PROV_ONLY,
			 hints, &info);
	if (ret) {
		sprintf(err_buf, "fi_getinfo returned %d - %s",
			-ret, fi_strerror(-ret));
		fprintf(stderr, "%s", err_buf);
		_exit(EXIT_FAILURE);
	}

	ret = serialize_nic_list(info);
	if (ret) {
		fprintf(stderr, "%s", err_buf);
		_exit(EXIT_FAILURE);
	}

        fi_freeinfo(info);
	_exit(EXIT_SUCCESS);
}

static int get_nic_list(char nics_out[][MAX_NIC_NAME_LEN],
			int *total_count_out, int *unique_count_out)
{
	int stdout_pipe[2], stderr_pipe[2];
	pid_t pid;
        char buffer[MAX_NIC_LIST_SIZE];
	ssize_t n;
	int status;
	int ret;

	if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
		sprintf(err_buf, "pipe() failed");
		return -FI_EIO;
	}

	pid = fork();
	if (pid < 0) {
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);
		close(stderr_pipe[0]);
		close(stderr_pipe[1]);
		sprintf(err_buf, "fork() failed");
		return -FI_EIO;
	}

	if (pid == 0) {
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);

		dup2(stdout_pipe[1], STDOUT_FILENO);
		dup2(stderr_pipe[1], STDERR_FILENO);
		close(stdout_pipe[1]);
		close(stderr_pipe[1]);

		getinfo_wrapper();
	}

	close(stdout_pipe[1]);
	close(stderr_pipe[1]);

	if (waitpid(pid, &status, 0) < 0) {
		sprintf(err_buf, "waitpid() failed");
		return -FI_EIO;
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS)
		return -FI_EOTHER;
        
        n = read(stdout_pipe[0], buffer, sizeof(buffer) - 1);
	buffer[n > 0 ? n : 0] = '\0';
	n = read(stderr_pipe[0], err_buf, sizeof(err_buf) - 1);
	err_buf[n > 0 ? n : 0] = '\0';
	ret = deserialize_nic_list(buffer, nics_out, total_count_out, unique_count_out);
	if (ret) return ret;

	close(stdout_pipe[0]);
	close(stderr_pipe[0]);

	return ret;
}

static int nic_affinity_unit_test(ft_nic_affinity_init init,
				   ft_nic_affinity_test test)
{
	char current_nics[MAX_NICS][MAX_NIC_NAME_LEN];
	int current_total_count, current_count;
	int ret;

	if (init) {
		ret = init(hints);
		if (ret) return ret;
	}

	ret = get_nic_list(current_nics, &current_total_count, &current_count);
	if (ret) return ret;

	if (baseline_total_count != current_total_count) {
		sprintf(err_buf, "Total NIC count mismatch: baseline has %d, current has %d",
			baseline_total_count, current_total_count);
		return EXIT_FAILURE;
	}

        if (baseline_count != current_count) {
		sprintf(err_buf, "Unique NIC count mismatch: baseline has %d, current has %d",
			baseline_count, current_count);
		return EXIT_FAILURE;
	}

	if (test) {
		ret = test(current_nics);
		if (ret) return ret;
	}

        cleanup_nic_affinity_test();
	return 0;
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
nic_affinity_test(none_sanity, "Test none policy for sanity",
		  init_none,
		  check_identical_list)
nic_affinity_test(manual_sanity, "Test manual policy for sanity",
		  init_manual,
		  check_same_nics)
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
		  check_same_nics)
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
	int failed, cleanup_ret, ret;
	int op;

	struct test_entry nic_affinity_tests[] = {
                TEST_ENTRY_NIC_AFFINITY(none_sanity),
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

	cleanup_nic_affinity_test();
	ret = get_nic_list(baseline_nics, &baseline_total_count, &baseline_count);
	if (ret) {
		fprintf(stderr, "Failed to capture baseline: %s\n", err_buf);
		fi_freeinfo(hints);
		return EXIT_FAILURE;
	}

	if (baseline_count == 0) {
		fprintf(stderr, "No NICs found in baseline\n");
		fi_freeinfo(hints);
		return EXIT_FAILURE;
	}

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

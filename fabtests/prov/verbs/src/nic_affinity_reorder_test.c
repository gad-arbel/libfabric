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
#include <sys/stat.h>
#include <unistd.h>

#include <rdma/fi_errno.h>

#include "shared.h"
#include "unit_common.h"
#include "jsmn.h"

#define OFI_CORE_PROV_ONLY      (1ULL << 59)
#define MAX_NICS                64
#define MAX_NIC_NAME_LEN        64
#define MAX_NIC_LIST_SIZE       (MAX_NICS * (MAX_NIC_NAME_LEN + 1) + 16)

/* JSON test config support */
struct affinity_test_config {
	char *name;
	char *policy;
	char *config_file;
	char *device;
	char **expected_nic_order;
	int num_nics;
};

static struct affinity_test_config *test_configs = NULL;
static int num_test_configs = 0;

/* Helper to extract JSON string value */
static char *json_token_string(const char *json, jsmntok_t *tok)
{
	int len = tok->end - tok->start;
	char *str = malloc(len + 1);
	if (!str)
		return NULL;
	memcpy(str, json + tok->start, len);
	str[len] = '\0';
	return str;
}

/* Helper to compare JSON token with string */
static int json_token_equals(const char *json, jsmntok_t *tok, const char *str)
{
	int len = tok->end - tok->start;
	return (strlen(str) == (size_t)len &&
		strncmp(json + tok->start, str, len) == 0);
}

/* Parse array of strings from JSON */
static int parse_string_array(const char *json, jsmntok_t *tokens, int idx,
			       char ***arr, int *count)
{
	jsmntok_t *array_tok = &tokens[idx];
	int i;

	if (array_tok->type != JSMN_ARRAY)
		return -1;

	*count = array_tok->size;
	*arr = calloc(*count, sizeof(char *));
	if (!*arr)
		return -1;

	for (i = 0; i < *count; i++) {
		(*arr)[i] = json_token_string(json, &tokens[idx + 1 + i]);
		if (!(*arr)[i])
			return -1;
	}

	return 0;
}

/* Parse environment variables object from JSON
 * Fixed structure: always 3 key-value pairs in this order:
 *   1. FI_VERBS_NIC_AFFINITY_POLICY
 *   2. FI_VERBS_AFFINITY_DEVICE
 *   3. FI_VERBS_NIC_AFFINITY_CONFIG
 */
static int parse_env_object(const char *json, jsmntok_t *tokens, int idx,
			     struct affinity_test_config *config)
{
	jsmntok_t *obj_tok = &tokens[idx];

	if (obj_tok->type != JSMN_OBJECT || obj_tok->size != 3)
		return -1;

	/* Token layout:
	 * idx+0: env object
	 * idx+1: key1, idx+2: val1
	 * idx+3: key2, idx+4: val2
	 * idx+5: key3, idx+6: val3
	 */
	config->policy = json_token_string(json, &tokens[idx + 2]);
	config->device = json_token_string(json, &tokens[idx + 4]);
	config->config_file = json_token_string(json, &tokens[idx + 6]);

	if (!config->policy || !config->device || !config->config_file)
		return -1;

	return 0;
}

/* Parse expected results object from JSON
 * Fixed structure: always 1 key-value pair:
 *   1. nic_order (array of strings)
 */
static int parse_expected_object(const char *json, jsmntok_t *tokens, int idx,
				 struct affinity_test_config *config)
{
	jsmntok_t *obj_tok = &tokens[idx];

	if (obj_tok->type != JSMN_OBJECT || obj_tok->size != 1)
		return -1;

	/* Token layout:
	 * idx+0: expected object
	 * idx+1: "nic_order" key, idx+2: array token
	 * idx+3...: array elements
	 */

	/* Parse nic_order array */
	if (parse_string_array(json, tokens, idx + 2,
			       &config->expected_nic_order,
			       &config->num_nics) < 0)
		return -1;

	return 0;
}

/* Parse single test object from JSON
 * Fixed structure: always 3 key-value pairs in this order:
 *   1. name (string)
 *   2. env (object with 3 key-value pairs)
 *   3. expected (object with 1 key-value pair: nic_order array)
 */
static int parse_test_object(const char *json, jsmntok_t *tokens, int idx,
			      struct affinity_test_config *config)
{
	jsmntok_t *obj_tok = &tokens[idx];

	if (obj_tok->type != JSMN_OBJECT || obj_tok->size != 3)
		return -1;

	memset(config, 0, sizeof(*config));

	/* Token layout:
	 * idx+0: test object
	 * idx+1: "name" key, idx+2: name value
	 * idx+3: "env" key, idx+4: env object (with 3 kv pairs = 1 + 3*2 = 7 tokens)
	 * idx+11: "expected" key, idx+12: expected object (with 1 kv pair: array)
	 */

	config->name = json_token_string(json, &tokens[idx + 2]);
	if (!config->name)
		return -1;

	if (parse_env_object(json, tokens, idx + 4, config) < 0)
		return -1;

	if (parse_expected_object(json, tokens, idx + 11, config) < 0)
		return -1;

	return 0;
}

/* Load JSON test configuration file */
static int load_json_test_config(const char *config_path)
{
	FILE *fp = NULL;
	struct stat sb;
	char *json = NULL;
	jsmn_parser parser;
	jsmntok_t *tokens = NULL;
	int num_tokens, ret = -1;
	int i, j, tests_array_idx = -1;

	fp = fopen(config_path, "r");
	if (!fp) {
		fprintf(stderr, "Failed to open config file: %s\n", config_path);
		return -1;
	}

	if (stat(config_path, &sb) < 0) {
		fprintf(stderr, "Failed to stat config file\n");
		goto out;
	}

	json = malloc(sb.st_size + 1);
	if (!json) {
		fprintf(stderr, "Failed to allocate memory for JSON\n");
		goto out;
	}

	if (fread(json, sb.st_size, 1, fp) != 1) {
		fprintf(stderr, "Failed to read config file\n");
		goto out;
	}
	json[sb.st_size] = '\0';

	jsmn_init(&parser);
	num_tokens = jsmn_parse(&parser, json, sb.st_size, NULL, 0);
	if (num_tokens < 0) {
		fprintf(stderr, "Failed to parse JSON (pass 1): %d\n", num_tokens);
		goto out;
	}

	tokens = malloc(sizeof(jsmntok_t) * num_tokens);
	if (!tokens) {
		fprintf(stderr, "Failed to allocate memory for tokens\n");
		goto out;
	}

	jsmn_init(&parser);
	ret = jsmn_parse(&parser, json, sb.st_size, tokens, num_tokens);
	if (ret != num_tokens) {
		fprintf(stderr, "Failed to parse JSON (pass 2)\n");
		ret = -1;
		goto out;
	}

	/* Find "tests" array in root object */
	if (tokens[0].type != JSMN_OBJECT) {
		fprintf(stderr, "Root element must be an object\n");
		ret = -1;
		goto out;
	}

	for (i = 1; i < tokens[0].size * 2; i += 2) {
		if (json_token_equals(json, &tokens[i], "tests")) {
			tests_array_idx = i + 1;
			break;
		}
	}

	if (tests_array_idx < 0 || tokens[tests_array_idx].type != JSMN_ARRAY) {
		fprintf(stderr, "No 'tests' array found in JSON\n");
		ret = -1;
		goto out;
	}

	num_test_configs = tokens[tests_array_idx].size;
	test_configs = calloc(num_test_configs, sizeof(struct affinity_test_config));
	if (!test_configs) {
		fprintf(stderr, "Failed to allocate memory for test configs\n");
		ret = -1;
		goto out;
	}

	/* Parse each test object
	 * Each test object has fixed structure:
	 *   - test object itself: 1 token
	 *   - name kv: 2 tokens
	 *   - env kv: 2 + 6 tokens (key + object + 3 kv pairs)
	 *   - expected kv: 2 + 2 + N tokens (key + object + nic_order kv + array + N elements)
	 *
	 * Total per test = 1 + 2 + 8 + 4 + N = 15 + N tokens
	 */
	j = tests_array_idx + 1;
	for (i = 0; i < num_test_configs; i++) {
		if (parse_test_object(json, tokens, j, &test_configs[i]) < 0) {
			fprintf(stderr, "Failed to parse test object %d\n", i);
			ret = -1;
			goto out;
		}
		/* Skip to next test object: 15 fixed tokens + N nic names */
		j += 15 + test_configs[i].num_nics;
	}

	printf("Loaded %d test configurations from %s\n", num_test_configs, config_path);
	ret = 0;

out:
	if (tokens)
		free(tokens);
	if (json)
		free(json);
	if (fp)
		fclose(fp);
	return ret;
}

/* Free loaded test configs */
static void free_test_configs(void)
{
	int i, j;

	if (!test_configs)
		return;

	for (i = 0; i < num_test_configs; i++) {
		free(test_configs[i].name);
		free(test_configs[i].policy);
		free(test_configs[i].config_file);
		free(test_configs[i].device);
		if (test_configs[i].expected_nic_order) {
			for (j = 0; j < test_configs[i].num_nics; j++)
				free(test_configs[i].expected_nic_order[j]);
			free(test_configs[i].expected_nic_order);
		}
	}
	free(test_configs);
	test_configs = NULL;
	num_test_configs = 0;
}

static const char *get_nic_name(struct fi_info *info)
{
	if (info->nic && info->nic->device_attr && info->nic->device_attr->name)
		return info->nic->device_attr->name;
	return NULL;
}

/* Serialize NIC list to stdout for parent process - runs in child process */
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
			fprintf(stderr, "NIC name too long: %s (max %d)", name,
				MAX_NIC_NAME_LEN - 1);
			return -FI_ENOMEM;
		}

		ret = snprintf(buffer + offset, sizeof(buffer) - offset, "%s,", name);
		if (ret < 0 || (size_t)ret >= sizeof(buffer) - offset) {
			fprintf(stderr, "Buffer overflow serializing NIC list");
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

/* Deserialize NIC list from child process output */
static int deserialize_nic_list(const char *serialized, char nics[][MAX_NIC_NAME_LEN],
				int *total_count, int *unique_count)
{
	char buffer[MAX_NIC_LIST_SIZE];
	char *token, *saveptr;
	char *first_colon, *second_colon;
	int i = 0;

	first_colon = strchr(serialized, ':');
	if (!first_colon) {
		fprintf(stderr, "Invalid format in serialized output");
		return -FI_EINVAL;
	}

	second_colon = strchr(first_colon + 1, ':');
	if (!second_colon) {
		fprintf(stderr, "Invalid format in serialized output (missing second colon)");
		return -FI_EINVAL;
	}

	*total_count = atoi(serialized);
	*unique_count = atoi(first_colon + 1);

	strncpy(buffer, second_colon + 1, sizeof(buffer) - 1);
	buffer[sizeof(buffer) - 1] = '\0';

	token = strtok_r(buffer, ",", &saveptr);
	while (token && i < MAX_NICS) {
		strncpy(nics[i], token, MAX_NIC_NAME_LEN);
		nics[i][MAX_NIC_NAME_LEN - 1] = '\0';
		i++;
		token = strtok_r(NULL, ",", &saveptr);
	}

	if (i != *unique_count) {
		fprintf(stderr, "Unique count mismatch: expected %d, got %d", *unique_count, i);
		return -FI_EINVAL;
	}

	return 0;
}

/* Fork and run fi_getinfo with environment variables set - runs in child */
static void getinfo_wrapper(struct fi_info *hints)
{
	struct fi_info *info = NULL;
	int ret;

	ret = fi_getinfo(FT_FIVERSION, NULL, NULL, OFI_CORE_PROV_ONLY,
			 hints, &info);
	if (ret) {
		fprintf(stderr, "fi_getinfo returned %d - %s",
			-ret, fi_strerror(-ret));
		_exit(EXIT_FAILURE);
	}

	ret = serialize_nic_list(info);
	if (ret) {
		_exit(EXIT_FAILURE);
	}

	fi_freeinfo(info);
	_exit(EXIT_SUCCESS);
}

/* Get NIC list by forking to ensure fresh environment variable reading */
static int get_nic_list(char nics_out[][MAX_NIC_NAME_LEN],
			int *total_count_out, int *unique_count_out,
			struct fi_info *hints)
{
	int stdout_pipe[2], stderr_pipe[2];
	pid_t pid;
	char buffer[MAX_NIC_LIST_SIZE];
	char err_buffer[512];
	ssize_t n;
	int status;
	int ret;

	if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
		fprintf(stderr, "pipe() failed");
		return -FI_EIO;
	}

	pid = fork();
	if (pid < 0) {
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);
		close(stderr_pipe[0]);
		close(stderr_pipe[1]);
		fprintf(stderr, "fork() failed");
		return -FI_EIO;
	}

	if (pid == 0) {
		/* Child process */
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);

		dup2(stdout_pipe[1], STDOUT_FILENO);
		dup2(stderr_pipe[1], STDERR_FILENO);
		close(stdout_pipe[1]);
		close(stderr_pipe[1]);

		getinfo_wrapper(hints);
	}

	/* Parent process */
	close(stdout_pipe[1]);
	close(stderr_pipe[1]);

	if (waitpid(pid, &status, 0) < 0) {
		fprintf(stderr, "waitpid() failed");
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		return -FI_EIO;
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
		n = read(stderr_pipe[0], err_buffer, sizeof(err_buffer) - 1);
		err_buffer[n > 0 ? n : 0] = '\0';
		if (n > 0)
			fprintf(stderr, "Child process error: %s\n", err_buffer);
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		return -FI_EOTHER;
	}

	n = read(stdout_pipe[0], buffer, sizeof(buffer) - 1);
	buffer[n > 0 ? n : 0] = '\0';

	ret = deserialize_nic_list(buffer, nics_out, total_count_out, unique_count_out);
	if (ret) {
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		return ret;
	}

	close(stdout_pipe[0]);
	close(stderr_pipe[0]);

	return 0;
}

/* Compare two NIC lists for identical order */
static int check_identical_list(char expected[][MAX_NIC_NAME_LEN], int expected_count,
				 char actual[][MAX_NIC_NAME_LEN], int actual_count)
{
	int i;

	if (expected_count != actual_count) {
		fprintf(stderr, "  FAIL: Expected %d NICs, got %d\n",
			expected_count, actual_count);
		return -1;
	}

	for (i = 0; i < expected_count; i++) {
		if (strcmp(expected[i], actual[i]) != 0) {
			fprintf(stderr, "  FAIL: NIC order mismatch at index %d: expected %s, got %s\n",
				i, expected[i], actual[i]);
			return -1;
		}
	}

	return 0;
}

/* Run a single test from JSON config */
static int run_json_test(struct affinity_test_config *config, struct fi_info *hints)
{
	char actual_nics[MAX_NICS][MAX_NIC_NAME_LEN];
	char expected_nics[MAX_NICS][MAX_NIC_NAME_LEN];
	int actual_total_count, actual_count;
	int ret, i;

	printf("\nRunning test: %s\n", config->name);

	for (i = 0; i < config->num_nics && i < MAX_NICS; i++) {
		strncpy(expected_nics[i], config->expected_nic_order[i], MAX_NIC_NAME_LEN);
		expected_nics[i][MAX_NIC_NAME_LEN - 1] = '\0';
	}

	setenv("FI_VERBS_NIC_AFFINITY_POLICY", config->policy, 1);
	setenv("FI_VERBS_NIC_AFFINITY_CONFIG", config->config_file, 1);
	setenv("FI_VERBS_AFFINITY_DEVICE", config->device, 1);

	ret = get_nic_list(actual_nics, &actual_total_count, &actual_count, hints);
	if (ret) {
		fprintf(stderr, "  FAIL: Failed to get NIC list\n");
		goto cleanup;
	}

	ret = check_identical_list(expected_nics, config->num_nics,
				    actual_nics, actual_count);
	if (ret == 0)
		printf("  PASS\n");

cleanup:
	/* Clean up environment */
	unsetenv("FI_VERBS_NIC_AFFINITY_POLICY");
	unsetenv("FI_VERBS_NIC_AFFINITY_CONFIG");
	unsetenv("FI_VERBS_AFFINITY_DEVICE");

	return ret;
}

static void usage(char *name)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "  %s -j <config.json> [-h]\n\n", name);
	fprintf(stderr, "NIC reorder tests for verbs GPU-NIC affinity feature\n\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -j <config.json>    JSON config file with test cases (required)\n");
	fprintf(stderr, "  -h                  display this help output\n");
}

int main(int argc, char **argv)
{
	int op, i, failed = 0;
	char *json_config_path = NULL;
	struct fi_info *hints = NULL;

        hints = fi_allocinfo();
	if (!hints)
		return EXIT_FAILURE;

	while ((op = getopt(argc, argv, "hj:")) != -1) {
		switch (op) {
		case 'j':
			json_config_path = strdup(optarg);
			if (!json_config_path) {
				fprintf(stderr, "Failed to allocate memory for config path\n");
				return EXIT_FAILURE;
			}
			break;
		case '?':
		case 'h':
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (!json_config_path) {
		fprintf(stderr, "Error: JSON config file is required (-j option)\n\n");
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	hints->fabric_attr->prov_name = strdup("verbs");
	if (!hints->fabric_attr->prov_name) {
		fprintf(stderr, "Failed to set provider name\n");
		fi_freeinfo(hints);
		free(json_config_path);
		return EXIT_FAILURE;
	}

	hints->mode = ~0;

	/* Load JSON configuration */
	if (load_json_test_config(json_config_path) < 0) {
		fprintf(stderr, "Failed to load JSON config from %s\n", json_config_path);
		fi_freeinfo(hints);
		free(json_config_path);
		return EXIT_FAILURE;
	}
	free(json_config_path);

	/* Run all tests from JSON */
	for (i = 0; i < num_test_configs; i++) {
		if (run_json_test(&test_configs[i], hints) < 0)
			failed++;
	}

	if (failed > 0) {
		printf("Summary: %d/%d tests failed\n", failed, num_test_configs);
	} else {
		printf("Summary: all %d tests passed\n", num_test_configs);
	}

	free_test_configs();
	fi_freeinfo(hints);

	return (failed > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}

#define _POSIX_C_SOURCE 200809L
#include "agent/tools/file_system/edit_file.h"
#include "agent/tools/file_system/write_file.h"
#include "agent/tools/file_system/read_file.h"
#include "agent/tools/file_system/search.h"
#include "agent/tools/run_command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)

static void release(ToolOutput *output) {
  if (output != NULL) { free(output->content); free(output); }
}

static cJSON *request(const char *path) {
  cJSON *args = cJSON_CreateObject();
  if (args != NULL && !cJSON_AddStringToObject(args, "path", path)) {
    cJSON_Delete(args); return NULL;
  }
  return args;
}

static int file_tests(void) {
  ToolOutput *output = NULL;
  AgentError error = {0};
  cJSON *args = request("source.c");
  CHECK(args && cJSON_AddStringToObject(args, "content", "int value = 1;\n"));
  CHECK(cJSON_AddStringToObject(args, "mode", "create"));
  CHECK(write_file(NULL, args, &output, &error) == AGENT_OK);
  release(output);
  CHECK(write_file(NULL, args, &output, &error) == AGENT_ERR_IO && output == NULL);
  CHECK(error.message[0] != '\0');
  cJSON_Delete(args);
  CHECK(chmod("source.c", 0750) == 0);
  args = request("source.c");
  CHECK(args && cJSON_AddStringToObject(args, "old_text", "value = 1"));
  CHECK(cJSON_AddStringToObject(args, "new_text", "value = 42"));
  CHECK(edit_file(NULL, args, &output, &error) == AGENT_OK);
  release(output);
  struct stat info;
  CHECK(stat("source.c", &info) == 0 && (info.st_mode & 0777) == 0750);
  CHECK(read_file(NULL, args, &output, &error) == AGENT_OK);
  CHECK(strcmp(output->content, "int value = 42;\n") == 0);
  release(output);
  CHECK(edit_file(NULL, args, &output, &error) == AGENT_ERR_INVAILD_ARGUMENT && output == NULL);
  cJSON_Delete(args);

  args = request("source.c");
  CHECK(args && cJSON_AddStringToObject(args, "content", "aaaa"));
  CHECK(cJSON_AddStringToObject(args, "mode", "overwrite"));
  CHECK(write_file(NULL, args, &output, &error) == AGENT_OK);
  release(output); cJSON_Delete(args);
  args = request("source.c");
  CHECK(args && cJSON_AddStringToObject(args, "old_text", "aaa"));
  CHECK(cJSON_AddStringToObject(args, "new_text", "b"));
  CHECK(edit_file(NULL, args, &output, &error) == AGENT_ERR_INVAILD_ARGUMENT && output == NULL);
  CHECK(read_file(NULL, args, &output, &error) == AGENT_OK);
  CHECK(strcmp(output->content, "aaaa") == 0);
  release(output); cJSON_Delete(args);

  args = request("source.c");
  CHECK(args && cJSON_AddStringToObject(args, "old_text", "aaaa"));
  CHECK(cJSON_AddStringToObject(args, "new_text", "\xe4\xb8\xad\xe6\x96\x87"));
  CHECK(edit_file(NULL, args, &output, &error) == AGENT_OK);
  release(output);
  CHECK(read_file(NULL, args, &output, &error) == AGENT_OK);
  CHECK(strcmp(output->content, "\xe4\xb8\xad\xe6\x96\x87") == 0);
  release(output);
  CHECK(cJSON_ReplaceItemInObjectCaseSensitive(args, "old_text", cJSON_CreateString("\xb8")));
  CHECK(edit_file(NULL, args, &output, &error) == AGENT_ERR_INVAILD_ARGUMENT && output == NULL);
  cJSON_Delete(args);

  args = request("missing");
  CHECK(args && cJSON_AddStringToObject(args, "content", ""));
  CHECK(cJSON_AddStringToObject(args, "mode", "overwrite"));
  CHECK(write_file(NULL, args, &output, &error) == AGENT_ERR_IO && output == NULL);
  CHECK(cJSON_ReplaceItemInObjectCaseSensitive(args, "mode", cJSON_CreateString("create")));
  CHECK(write_file(NULL, args, &output, &error) == AGENT_OK);
  release(output);
  CHECK(read_file(NULL, args, &output, &error) == AGENT_OK && strcmp(output->content, "") == 0);
  release(output); cJSON_Delete(args);
  CHECK(unlink("missing") == 0);

  CHECK(symlink("source.c", "link.c") == 0);
  args = request("link.c");
  CHECK(args && cJSON_AddStringToObject(args, "content", "wrong"));
  CHECK(cJSON_AddStringToObject(args, "mode", "overwrite"));
  CHECK(write_file(NULL, args, &output, &error) == AGENT_ERR_INVAILD_ARGUMENT && output == NULL);
  CHECK(lstat("link.c", &info) == 0 && S_ISLNK(info.st_mode));
  cJSON_Delete(args);
  CHECK(unlink("link.c") == 0);
  CHECK(unlink("source.c") == 0);
  return 0;
}

static int command_test(const char *input, const char *stdout_text, int exit_code, int is_error) {
  cJSON *args = cJSON_Parse(input);
  ToolOutput *output = NULL;
  AgentError error = {0};
  CHECK(args != NULL);
  CHECK(run_command(NULL, args, &output, &error) == AGENT_OK);
  CHECK(output != NULL && output->is_error == is_error);
  cJSON *json = cJSON_Parse(output->content);
  CHECK(json != NULL);
  CHECK(cJSON_GetObjectItemCaseSensitive(json, "exit_code")->valueint == exit_code);
  CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "stdout")), stdout_text) == 0);
  cJSON_Delete(json); release(output); cJSON_Delete(args);
  return 0;
}

static int process_tests(void) {
  CHECK(command_test("{\"command\":\"printf 'hello'; printf 'error' >&2; exit 7\"}", "hello", 7, 1) == 0);
  CHECK(command_test("{\"command\":\"read value; printf '%s' \\\"$?\\\"\"}", "1", 0, 0) == 0);
  CHECK(command_test("{\"command\":\"printf ok\",\"cwd\":\"/nonexistent-tiny-agent-path\"}", "", 126, 1) == 0);
  CHECK(command_test("{\"command\":\"pwd\",\"cwd\":\"/\"}", "/\n", 0, 0) == 0);
  CHECK(command_test("{\"command\":\"printf '\\\\377\\\\000'\"}", "\xef\xbf\xbd\xef\xbf\xbd", 0, 0) == 0);
  cJSON *args = cJSON_Parse("{\"command\":\"while :; do printf 1234567890; printf error >&2; done\",\"timeout_ms\":50,\"max_output_bytes\":64}");
  ToolOutput *output = NULL;
  AgentError error = {0};
  struct timespec start, end;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
  CHECK(run_command(NULL, args, &output, &error) == AGENT_OK && output->is_error);
  CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
  CHECK(end.tv_sec - start.tv_sec < 3);
  cJSON *json = cJSON_Parse(output->content);
  CHECK(json && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "timed_out")));
  CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "truncated")));
  CHECK(strlen(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "stdout"))) +
        strlen(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "stderr"))) == 64);
  cJSON_Delete(json); release(output); cJSON_Delete(args);

  // Shell and descendants can both hold capture pipes open after a command.
  args = cJSON_Parse("{\"command\":\"sleep 10 & printf early\",\"timeout_ms\":50}");
  CHECK(run_command(NULL, args, &output, &error) == AGENT_OK && output->is_error);
  json = cJSON_Parse(output->content);
  CHECK(json && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "timed_out")));
  cJSON_Delete(json); release(output); cJSON_Delete(args);
  int child_status;
  CHECK(waitpid(-1, &child_status, WNOHANG) == -1 && errno == ECHILD);

  args = cJSON_Parse("{\"command\":\"true\",\"timeout_ms\":1.5}");
  CHECK(run_command(NULL, args, &output, &error) == AGENT_ERR_INVAILD_ARGUMENT && output == NULL);
  cJSON_Delete(args);
  return 0;
}

static int search_tests(void) {
  FILE *file = fopen("one.c", "w");
  CHECK(file && fputs("first\nneedle\nneedle\n", file) >= 0 && fclose(file) == 0);
  file = fopen("two.txt", "w");
  CHECK(file && fputs("needle\n", file) >= 0 && fclose(file) == 0);
  cJSON *args = cJSON_Parse("{\"pattern\":\"needle\",\"glob\":\"*.c\",\"max_results\":1}");
  ToolOutput *output = NULL;
  AgentError error = {0};
  CHECK(search(NULL, args, &output, &error) == AGENT_OK && !output->is_error);
  cJSON *json = cJSON_Parse(output->content);
  CHECK(json != NULL);
  cJSON *matches = cJSON_GetObjectItemCaseSensitive(json, "matches");
  CHECK(cJSON_GetArraySize(matches) == 1 && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "truncated")));
  cJSON *match = cJSON_GetArrayItem(matches, 0);
  CHECK(strstr(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(match, "path")), "one.c"));
  CHECK(cJSON_GetObjectItemCaseSensitive(match, "line")->valueint == 2);
  CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(match, "text")), "needle") == 0);
  cJSON_Delete(json); release(output); cJSON_Delete(args);
  args = cJSON_Parse("{\"pattern\":\"[\",\"fixed_string\":true}");
  CHECK(search(NULL, args, &output, &error) == AGENT_OK && !output->is_error);
  json = cJSON_Parse(output->content);
  CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(json, "matches")) == 0);
  cJSON_Delete(json); release(output); cJSON_Delete(args);
  args = cJSON_Parse("{\"pattern\":\"[\"}");
  CHECK(search(NULL, args, &output, &error) == AGENT_OK && output->is_error);
  release(output); cJSON_Delete(args);
  CHECK(unlink("one.c") == 0 && unlink("two.txt") == 0);
  return 0;
}

int main(void) {
  char directory[] = "/tmp/tiny-agent-tools-XXXXXX";
  CHECK(mkdtemp(directory) != NULL && chdir(directory) == 0);
  CHECK(file_tests() == 0);
  CHECK(process_tests() == 0);
  CHECK(search_tests() == 0);
  ToolRegistry *registry = NULL;
  AgentError error = {0};
  CHECK(tool_registry_init(&registry) == AGENT_OK);
  CHECK(write_file_register(registry, &error) == AGENT_OK);
  CHECK(edit_file_register(registry, &error) == AGENT_OK);
  CHECK(search_register(registry, &error) == AGENT_OK);
  CHECK(run_command_register(registry, &error) == AGENT_OK);
  CHECK(registry->count == 4);
  for (size_t i = 0; i < registry->count; ++i) {
    cJSON *schema = cJSON_Parse(registry->tools[i]->parameters_json);
    CHECK(schema != NULL);
    cJSON_Delete(schema);
  }
  CHECK(tool_registry_destory(registry) == AGENT_OK);
  CHECK(chdir("/") == 0 && rmdir(directory) == 0);
  return 0;
}

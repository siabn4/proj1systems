/*
 * minibash - an open-ended subset of bash
 *
 * Developed by Godmar Back for CS 3214 Fall 2025 
 * Virginia Tech.
 */
#define _GNU_SOURCE    1
#include <stdio.h>
#include <readline/readline.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <time.h>

#include <tree_sitter/api.h>
#include "tree_sitter/tree-sitter-bash.h"
#include "ts_symbols.h"
/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"
// Included functions 
#include <spawn.h>

#include "hashtable.h"
#include "signal_support.h"
#include "utils.h"
#include "list.h"
#include "ts_helpers.h"

/* These are field ids suitable for use in ts_node_child_by_field_id for certain rules. 
   e.g., to obtain the body of a while loop, you can use:
    TSNode body = ts_node_child_by_field_id(child, bodyId);
*/
static TSFieldId bodyId, redirectId, destinationId, valueId, nameId, conditionId;
static TSFieldId variableId;
static TSFieldId leftId, operatorId, rightId;

static char *input;         // to avoid passing the current input around
static TSParser *parser;    // a singleton parser instance 
static tommy_hashdyn shell_vars;        // a hash table containing the internal shell variables
static int last_exit_status = 0; // exit status of the completed foreground process

static void handle_child_status(pid_t pid, int status);
static char *read_script_from_fd(int readfd);
static void execute_script(char *script);
static char **grab_argv(TSNode command, const char *input, tommy_hashdyn *shell_vars, int last_status);
static void run_statement(TSNode child);
static void run_command(TSNode command);
static void run_pipeline(TSNode pipeline);
extern char **environ;


static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
        " -h            print this help\n",
        progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt */
static char *
build_prompt(void)
{
    return strdup("minibash> ");
}

/* Possible job status's to use.
 *
 * Some are specific to interactive job control which may not be needed
 * for this assignment.
 */
enum job_status {
    FOREGROUND,     /* job is running in foreground.  Only one job can be
                       in the foreground state. */
    BACKGROUND,     /* job is running in background */
    STOPPED,        /* job is stopped via SIGSTOP */
    NEEDSTERMINAL,  /* job is stopped because it was a background job
                       and requires exclusive terminal access */
    TERMINATED_VIA_EXIT,    /* job terminated via normal exit. */
    TERMINATED_VIA_SIGNAL   /* job terminated via signal. */
};

struct job {
    struct list_elem elem;   /* Link element for jobs list. */
    int     jid;             /* Job id. */
    enum job_status status;  /* Job status. */ 
    int  num_processes_alive;   /* The number of processes that we know to be alive */

    /* Add additional fields here as needed. */
    pid_t  *pids; // dynamic array of the pids in the job
    int     num_pids;   // number of pids in the job
    int     cap_pids;    // capacity of the pids array
    int     last_status; // the exit status 
};

/* Utility functions for job list management.
 * We use 2 data structures: 
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1<<16)
static struct list job_list;

static struct job *jid2job[MAXJOBS];

/* Return job corresponding to jid */
static struct job *
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}

static void 
add_pid_to_job(struct job *job, pid_t pid)
{
    // if at the capacity scale up 
    if (job->num_pids == job->cap_pids)
    {
        // if the capcity is starting at 0 then increase to 4
        if (job->cap_pids == 0)
        {
            job->cap_pids = 4;
        }
        // otherwise double the capacity 
        else    
            job->cap_pids *= 2;
        // reallocate the memory with the new capacity 
        job->pids = realloc(job->pids, sizeof(pid_t) * job->cap_pids);
    }
    // get the individual pid that was placed into the job
    job->pids[job->num_pids++] = pid;
}
 
static struct job *
find_job_by_pid(pid_t pid)
{
    struct list_elem *ele;
    ele = list_begin(&job_list);

    while ( ele != list_end(&job_list))
    {
        struct job *job = list_entry(ele, struct job, elem);

        for (int i = 0; i < job->num_pids; i++)
        {
            if (job->pids[i] == pid)
            {
                return job;
            }
        }
        ele = list_next(ele);
    }
    return NULL;
}

/* Allocate a new job, optionally adding it to the job list. */
static struct job *
allocate_job(bool includeinjoblist)
{
    struct job * job = malloc(sizeof *job);
    job->num_processes_alive = 0;
    job->jid = -1;
    job->pids =NULL;
    job->num_pids = 0;
    job->cap_pids = 0;
    job->last_status = 0;
    if (!includeinjoblist)
        return job;

    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++) {
        if (jid2job[i] == NULL) {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job, bool removeFromJobList)
{
    if (removeFromJobList) {
        int jid = job->jid;
        assert(jid != -1);
        assert(jid2job[jid] == job);
        list_remove(&job->elem);
        jid2job[jid]->jid = -1;
        jid2job[jid] = NULL;
    } else {
        assert(job->jid == -1);
    }
    /* add any other job cleanup here. */
    free(job->pids);
    free(job);
}


/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD 
 * signal may be delivered for multiple children that have 
 * exited. All of them need to be reaped.
 */
static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0) {
        handle_child_status(child, status);
    }
}

/* Wait for all processes in this job to complete, or for
 * this job to change status - for instance, for the job to no longer be
 * in the foreground.  (Pass the job's current status to wait for all
 * processes that are part of the job to complete.)
 *
 * You should call this function with fromstatus==FOREGROUND from where
 * you wait for jobs started without the &; you could use this function
 * if you were to implement the 'fg' command as well (job control only).
 *
 * You can also use this function to wait for background jobs (if you
 * were to implement the `wait` built-in.)
 * 
 * This function calls, and relies on a correct implementation of, the
 * handle_child_status function.  It must record the information obtained
 * from waitpid() for pid 'child', updating any job status changes that
 * may result.
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to the waited-for
 * job's status and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job_to_change_status(struct job *job, int fromstatus)
{
    assert(signal_is_blocked(SIGCHLD));

    while (job->status == fromstatus && job->num_processes_alive > 0) {
        int status;

        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
}


static void
handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));

    /* To be implemented. 
     * Step 1. Given the pid, determine which job this pid is a part of
     *         (how to do this is not part of the provided code.)
     * Step 2. Determine what status change occurred using the
     *         WIF*() macros.
     * Step 3. Update the job status accordingly, and adjust 
     *         num_processes_alive if appropriate.
     *         If a process was stopped, save the terminal state.
     */
    // find what job the pid belongs to
    struct job *job = find_job_by_pid(pid);
    if (job == NULL)
        return;

    // WIFEXITED when child is terminated normally 
    if (WIFEXITED(status))
    {
        // set the status to reflect exited
        job->last_status = WEXITSTATUS(status);
        //remove the process from the job
        job->num_processes_alive--;
        if (job->num_processes_alive == 0)
        {
            job->status = TERMINATED_VIA_EXIT;
        }
    }
    else if (WIFSIGNALED(status))
    {
        job->last_status = 128 + WTERMSIG(status);
        job->num_processes_alive--;
         if (job->num_processes_alive == 0)
        {
            job->status = TERMINATED_VIA_SIGNAL;
        }
    } 
    else if (WIFSTOPPED(status))
        job->status = STOPPED;
}

static char *
run_command_capture(TSNode command)
{
    char **argv = grab_argv(command, input, &shell_vars, last_exit_status);

    int pipefd[2];
    if (pipe(pipefd) == -1)
    {
        utils_error("pipe failed");
        return strdup("");
    }

    struct job *job = allocate_job(true);
    job->status = FOREGROUND;
    job->num_processes_alive = 1;

    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);

    /* Child stdout goes into the pipe. */
    posix_spawn_file_actions_adddup2(
        &file_actions, pipefd[1], STDOUT_FILENO);

    posix_spawn_file_actions_addclose(
        &file_actions, pipefd[0]);

    posix_spawn_file_actions_addclose(
        &file_actions, pipefd[1]);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);

    pid_t pid;
    int result = posix_spawnp(
        &pid,
        argv[0],
        &file_actions,
        &attr,
        argv,
        environ
    );

    posix_spawn_file_actions_destroy(&file_actions);
    posix_spawnattr_destroy(&attr);

    /* Parent does not write into the pipe. */
    close(pipefd[1]);

    if (result != 0)
    {
        close(pipefd[0]);

        for (int i = 0; argv[i] != NULL; i++)
            free(argv[i]);
        free(argv);

        delete_job(job, true);
        return strdup("");
    }

    add_pid_to_job(job, pid);

    /* Read everything the command wrote to stdout. */
    size_t capacity = 128;
    size_t length = 0;
    char *output = malloc(capacity);

    char buffer[128];
    ssize_t bytes;

    while ((bytes = read(pipefd[0], buffer, sizeof(buffer))) > 0)
    {
        if (length + bytes + 1 > capacity)
        {
            while (length + bytes + 1 > capacity)
                capacity *= 2;

            output = realloc(output, capacity);
        }

        memcpy(output + length, buffer, bytes);
        length += bytes;
    }

    close(pipefd[0]);

    wait_for_job_to_change_status(job, FOREGROUND);

    output[length] = '\0';

    /* Bash removes trailing newlines from command substitution. */
    while (length > 0 && output[length - 1] == '\n')
    {
        output[--length] = '\0';
    }

    delete_job(job, true);

    for (int i = 0; argv[i] != NULL; i++)
        free(argv[i]);
    free(argv);

    return output;
}

static char *
word_node_extractor(TSNode node, const char *input, tommy_hashdyn *shell_vars, int last_status)
{
    if (ts_node_is_null(node))
    {
        return strdup("");
    }

    TSSymbol sym = ts_node_symbol(node);

    if (sym == sym_raw_string)
    {
        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);
    

        // has to have at least 2 characters for the quotes 
        if (end - start >= 2)
        {
            // get the length of the string minus the quotes
            uint32_t length = end - start - 2;
            char *buf = malloc(length + 1);
            memcpy(buf, input + start + 1, length);
            buf[length] = '\0';
            return buf;
        }
        return ts_extract_node_text((char *)input, node);
    }   

    if (sym == sym_string || sym == sym_concatenation)
    {
        uint32_t child_count = ts_node_named_child_count(node);

        if (sym == sym_string && child_count == 0)
        {
            uint32_t start = ts_node_start_byte(node);
            uint32_t end = ts_node_end_byte(node);
            if (end - start >= 2)
            {
                uint32_t length = end - start - 2;
                char *result = malloc(length + 1);

                memcpy(result, input + start + 1, length);
                result[length] = '\0';

                return result;
            }
        }

        char *result = strdup("");

        for (uint32_t i = 0; i < child_count; i++)
        {
            TSNode child = ts_node_named_child(node, i);
            char *part = word_node_extractor(child, input, shell_vars, last_status);
            if (part)
            {
                result = utils_string_concat(result, part);
            }
        }
        return result;
    }

    if (sym == sym_expansion || sym == sym_simple_expansion)
    {
        TSNode var_name_node = ts_node_child_by_field_id(node, variableId);
        if (ts_node_is_null(var_name_node))
        {
            var_name_node = ts_node_named_child(node, 0);
        }

        char *var_name = ts_extract_node_text((char *)input, var_name_node);

        //strip leading characters like $
        char *lookup = var_name;
        if (lookup[0] == '$' && lookup[1] != '\0')
        {
            lookup++;
        }
        char *val = NULL;
        if (strcmp(lookup, "?") == 0)
        {
            char buf[16];
            //prevent buffer overflow 
            snprintf(buf, sizeof(buf), "%d", last_status);
            val = strdup(buf);
        }
        else if (strcmp(lookup, "$") == 0)
        {
            char buf[16];
            //prevent buffer overflow 
            snprintf(buf, sizeof(buf), "%d", (int)getpid());
            val = strdup(buf);
        }
        else
        {
            const char *env_val = hash_get(shell_vars, lookup);
            if (!env_val)
            {
                env_val = getenv(lookup);
            }
            if (env_val != NULL)
            {
                val = strdup(env_val);
            }
            else
            {
                val = strdup("");
            }
        }
        free(var_name);
        return val;
    }
    
    if (sym == sym_command_substitution)
    {
        TSNode command = ts_node_named_child(node, 0);
        if (ts_node_is_null(command))
        {
            return strdup("");
        }
        return run_command_capture(command);
    }
    return ts_extract_node_text((char *)input, node);
}

static char **
grab_argv(TSNode command, const char *input, tommy_hashdyn *shell_vars, int last_status)
{
    // gets all the tokens in the command 
    uint32_t num = ts_node_named_child_count(command);
    // allocate the pointers of each token and the null terminator 
    char **argv = malloc(sizeof(char *) * (num + 1));
    uint32_t argc = 0;

    // loop through all the tokens of the command
    for (uint32_t i = 0; i < num; i++)
    {
        // save the child containing the text
        TSNode child = ts_node_named_child(command, i);
        TSSymbol sym = ts_node_symbol(child);
        if (sym == sym_file_redirect || sym == sym_heredoc_redirect)
        { 
            continue; 
        }
        // extract the text from the child 
        argv[argc++] = word_node_extractor(child, input, shell_vars, last_status);
    }
    argv[argc] = NULL;

    return argv;
}

static void
run_list(TSNode list_node)
{
    TSNode left = ts_node_named_child(list_node, 0);
    TSNode right = ts_node_named_child(list_node, 1);

    bool is_and = false;
    bool is_or = false;

    uint32_t all_children = ts_node_named_child_count(list_node);
    for (uint32_t i = 0; i < all_children; i++)
    {
        TSNode c = ts_node_child(list_node, i);
        TSSymbol sym = ts_node_symbol(c);
        if (sym == anon_sym_AMP_AMP)
        {
            is_and = true;
        }
        else if (sym == anon_sym_PIPE_PIPE)
        {
            is_or = true;
        }
    }

    run_statement(left);
    int left_status = last_exit_status;

    if (is_and && left_status == 0)
    {
        run_statement(right);
    }
    if (is_or && left_status != 0)
    {
        run_statement(right);
    }
}


static void
run_statement(TSNode child)
{
    if (ts_node_symbol(child) == sym_command)
        {
            run_command(child);
        }
        // handles dealing with comments (test 1)
        else if (ts_node_symbol(child) == sym_comment)
        {
            // no output just continue 
        }
        else if (ts_node_symbol(child) == sym_variable_assignment)
        {
            TSNode name_node = ts_node_child_by_field_id(child, nameId);
            TSNode value_node = ts_node_child_by_field_id(child, valueId);

            char *name = ts_extract_node_text(input, name_node);
            char *value = word_node_extractor(
                value_node,
                input,
                &shell_vars,
                last_exit_status
            );
            hash_put(&shell_vars, name, value);
        }
        else if (ts_node_symbol(child) == sym_pipeline)
        {
            run_pipeline(child);
        }
        else if (ts_node_symbol(child) == sym_redirected_statement)
        {

        }
        else if (ts_node_symbol(child) == sym_list)
        {
            run_list(child);
        }
        else if (ts_node_symbol(child) == sym_if_statement)
        {
            // run if statment 
        }
        else if (ts_node_symbol(child) == sym_test_command)
        {
            // run test command 
        }
        else
        {
            // DO SOMETHING HERE
            printf("node type `%s` not implemented\n", ts_node_type(child));
        }
}

static void 
run_command(TSNode command)
{
    // save the arguments from the given command 
    char **argv = grab_argv(command, input, &shell_vars, last_exit_status);
    // create a new job for processes to be placed in 
    struct job *job = allocate_job(true);
    job->status = FOREGROUND;
    // job has to have at least 1 process to stay active
    job->num_processes_alive = 1;

    // create a posix spawn variable for files
    posix_spawn_file_actions_t file_actions;
    // initiate the file actions 
    posix_spawn_file_actions_init(&file_actions);

    posix_spawnattr_t attr;
    // initialize the attributes
    posix_spawnattr_init(&attr);
    // set the process group
    posix_spawnattr_setpgroup(&attr, 0);
    // set the flags for the command 
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);

    pid_t pid;
    int process = posix_spawnp(&pid, argv[0], &file_actions, &attr, argv, environ);

    // destroy the spawn object 
    posix_spawn_file_actions_destroy(&file_actions);
    posix_spawnattr_destroy(&attr);

    if (process != 0)
    {
        utils_error("posix_spawnp failed");
        for ( int i = 0; argv[i] != NULL; i++)
        {
            free(argv[i]);
        }
        free(argv);
        delete_job(job, true);
        return;
    }
    // add the pid of child to the job list
    add_pid_to_job(job, pid);

    // wiat for the process to longer be in the foreground meaning it complete
    wait_for_job_to_change_status(job, FOREGROUND);
    last_exit_status = job->last_status;
    delete_job(job, true);
    for ( int i = 0; argv[i] != NULL; i++)
    {
        free(argv[i]);
    }
    free(argv);

}

static void 
run_pipeline(TSNode pipeline)
{
    uint32_t nde = ts_node_named_child_count(pipeline);
    if (nde == 0)
    {
        return;
    }

    uint32_t num_pipes = nde - 1;

    int (*pipes)[2] = NULL;
    if ( nde > 1)
    {
        pipes = malloc(sizeof(int[2]) * num_pipes);
        for ( uint32_t i = 0; i < num_pipes; i++)
        {
            if (pipe(pipes[i]) != 0)
            {
                utils_error("Pipe failed");
                free(pipes);
                return;
            }
        }
    }

    struct job *job = allocate_job(true);
    job->status = FOREGROUND;
    job->num_processes_alive = 0;

    // process group leader
    pid_t leader_pid = -1;

    for (uint32_t i = 0; i < nde; i++)
    {
        TSNode cmd_node = ts_node_named_child(pipeline, i);
        char **argv = grab_argv(cmd_node, input, &shell_vars, last_exit_status);

        posix_spawn_file_actions_t file_actions;
        posix_spawn_file_actions_init(&file_actions);

        // not the first command
        if (i > 0)
        {
            //stdin comes from the previous pipe end 
            posix_spawn_file_actions_adddup2(&file_actions, pipes[i - 1][0], STDIN_FILENO);
        }
        if (i + 1 < nde)
        {
            posix_spawn_file_actions_adddup2(&file_actions, pipes[i][1], STDOUT_FILENO);
        }
        // close all pipes
        if (nde > 1)
        {
            for (uint32_t j = 0; j < num_pipes; j++)
            {
                posix_spawn_file_actions_addclose(&file_actions, pipes[j][0]);
                posix_spawn_file_actions_addclose(&file_actions, pipes[j][1]);
            }
        }

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);

        if (i == 0)
        {
            // set the first process as the froup leader 
            posix_spawnattr_setpgroup(&attr, 0);
        }
        else
        {
            posix_spawnattr_setpgroup(&attr, leader_pid);
        }
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);

        pid_t pid;

        int spawn_result = posix_spawnp(&pid, argv[0], &file_actions, &attr, argv, environ);

        posix_spawn_file_actions_destroy(&file_actions);
        posix_spawnattr_destroy(&attr);

        for ( int argument = 0; argv[argument] != NULL; argument++)
        {
            free(argv[argument]);
        }
        free(argv);

        if (spawn_result != 0)
        {
            utils_error("posix_spawnp failed");
            continue;
        }

        if ( i == 0)
        {
            leader_pid = pid;
        }

        add_pid_to_job(job, pid);
        job->num_processes_alive++;
    }

    if (nde > 1)
    {
        for ( uint pcount = 0 ; pcount < num_pipes; pcount++)
        {
            close(pipes[pcount][0]);
            close(pipes[pcount][1]);
        }
    }
    free(pipes);

    wait_for_job_to_change_status(job, FOREGROUND);
    last_exit_status = job->last_status;
    delete_job(job, true);
}

/*
 * Run a program.
 *
 * A program's named children are various types of statements which 
 * you can start implementing here.
 */
static void
run_program(TSNode program)
{
    uint32_t n = ts_node_named_child_count(program);
    for (uint32_t i = 0; i < n; i++) 
    {
        TSNode child = ts_node_named_child(program, i);
        run_statement(child);
    }
}

/*
 * Read a script from this (already opened) file descriptor,
 * return a newly allocated buffer.
 */
static char *
read_script_from_fd(int readfd)
{
    struct stat st;
    if (fstat(readfd, &st) != 0) {
        utils_error("Could not fstat input");
        return NULL;
    }

    char *userinput = malloc(st.st_size+1);
    if (read(readfd, userinput, st.st_size) != st.st_size) {
        utils_error("Could not read input");
        free(userinput);
        return NULL;
    }
    userinput[st.st_size] = 0;
    return userinput;
}

/* 
 * Execute the script whose content is provided in `script`
 */
static void 
execute_script(char *script)
{
    input = script;
    TSTree *tree = ts_parser_parse_string(parser, NULL, input, strlen(input));
    TSNode  program = ts_tree_root_node(tree);
    signal_block(SIGCHLD);
    run_program(program);
    signal_unblock(SIGCHLD);
    ts_tree_delete(tree);
}

int
main(int ac, char *av[])
{
    int opt;
    tommy_hashdyn_init(&shell_vars);
    srandom(time(NULL));

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0) {
        switch (opt) {
        case 'h':
            usage(av[0]);
            break;
        }
    }
    int nextarg = optind;

    parser = ts_parser_new();
    const TSLanguage *bash = tree_sitter_bash();
#define DEFINE_FIELD_ID(name) \
    name##Id = ts_language_field_id_for_name(bash, #name, strlen(#name))
    DEFINE_FIELD_ID(body);
    DEFINE_FIELD_ID(condition);
    DEFINE_FIELD_ID(name);
    DEFINE_FIELD_ID(right);
    DEFINE_FIELD_ID(left);
    DEFINE_FIELD_ID(operator);
    DEFINE_FIELD_ID(value);
    DEFINE_FIELD_ID(redirect);
    DEFINE_FIELD_ID(destination);
    DEFINE_FIELD_ID(variable);
    ts_parser_set_language(parser, bash);

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);


    /* Read/eval loop. */
    bool shouldexit = false;
    for (;;) {
        if (shouldexit)
            break;

        /* If you fail this assertion, you were about to enter readline()
         * while SIGCHLD is blocked.  This means that your shell would be
         * unable to receive SIGCHLD signals, and thus would be unable to
         * wait for background jobs that may finish while the
         * shell is sitting at the prompt waiting for user input.
         */
        assert(!signal_is_blocked(SIGCHLD));

        char *userinput = NULL;
        /* Do not output a prompt unless shell's stdin is a terminal */
        if (isatty(0) && av[nextarg] == NULL) {
            char *prompt = isatty(0) ? build_prompt() : NULL;
            userinput = readline(prompt);
            free (prompt);
            if (userinput == NULL)
                break;
        } else {
            int readfd = 0;
            if (av[nextarg] != NULL)
                readfd = open(av[nextarg], O_RDONLY);

            userinput = read_script_from_fd(readfd);
            close(readfd);
            if (userinput == NULL)
                utils_fatal_error("Could not read input");
            shouldexit = true;
        }
        execute_script(userinput);
        free(userinput);
    }

    /* 
     * Even though it is not necessary for the purposes of resource
     * reclamation, we free all allocated data structure prior to exiting
     * so that we can use valgrind's leak checker.
     */
    ts_parser_delete(parser);
    tommy_hashdyn_foreach(&shell_vars, hash_free);
    tommy_hashdyn_done(&shell_vars);
    return EXIT_SUCCESS;
}

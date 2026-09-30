cs3214 Fall 2026 - Project 1 

<h2>GitLab Repository:</h2>
https://git.cs.vt.edu/emersonbeck05/minibash

<h3>Custom Functions</h3>
These were all added to minibash.c:

<ul> 
    <li><b>Job and Process Management</b></li>
    <ul>
        <li>add_pid_to_job() adds a process to a job </li>
        <li>find_job_by_pid() finds the job a pid is in</li>
        <li>allocate_job()</li>
    </ul>
    <li><b>Command Execution</b></li>
    <ul>
        <li>grab_argv() gets arguments for the tree-sitter nodes</li>
        <li>word_node_extractor() extracts the values from tree-sitter nodes</li>
        <li>run_command() uses posix spawn for for command execution</li>
        <li>run_command_redirected() executes commands with file redirectors </li>
        <li>run_pipeline executes pipes</li>
        <li>run_pipeline_redirected() is for pipeline execution with file redirections</li>
        <li>run_command_capture() runs a command and cpatures its value </li>
        <li>run_pipeline_capture() executues a pipe and captures the output </li>
        <li>apply_file_redirect() configure input and output</li>
    </ul>
    <li><b>Shell</b></li>
    <ul>
        <li>run_list() handles && and || command lists</li>
        <li>run_if_statement() handles if, elif, and else clauses</li>
        <li>Run_for_statement() handles with for loops</li>
        <li>run_while_statement() handles with while loops</li>
        <li>run_group_command() for group statements</li>
        <li>condition_is_true() for determinig shell conditions</li>
        <li>run_test_command() for shell test expressions</li>
        <li>run_statement() which deals with directing the statements to the correct function</li>
        <li>run_program() to execute</li>
    </ul> 
    <li></li>

    
</ul>


How to build minibash
---------------------

1. Build libtommy
```
(cd tommyds; make)
```

2. Build libtree-sitter.a
```
(cd tree-sitter; make)
```

3. Build minibash

```
cd src
make
```

4. To use the tree-sitter CLI tool, it is recommended to
```
source env.sh
```

5. Test the tree-sitter tool with with
```
tree-sitter parse tests/001-comment.sh
```
(The `tree-sitter` example must be separately installed; ours is in
`~cs3214/bin`)

6. Build the tests

```
(cd tests; make)
```

7. On rlogin, you may run the tests by invoking 

```
minibash_driver.py
```

On rlogin, this will pick up `minibash_driver.py` installed in our class directory.

Use `../tests/minibash_driver.py` if you wish to run your copy of tests.


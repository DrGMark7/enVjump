Extend jenv with environment variable persistence and uv support.

Current jenv already supports:
- jenv set [ENV_NAME]
- jenv purge
- jenv status
- jenv install
- jenv uninstall
- ~/.config/jenv/config
- bash hook in ~/.bashrc

New requirements:

1. Add uv support

Commands:
- jenv set --uv
  Save current working directory and mark env type as uv.
  Detect venv path:
    a. If VIRTUAL_ENV is set, use that.
    b. Else if .venv/bin/activate exists in current directory, use "$PWD/.venv".
    c. Else fail with a helpful message telling user to run `uv venv` first or activate the venv.

- jenv set --conda [ENV_NAME]
  Explicit conda mode.
  If ENV_NAME is missing, use CONDA_DEFAULT_ENV, fallback to base.

- Existing `jenv set [ENV_NAME]` should remain backward compatible and mean conda mode.

Config should include:
  export JENV_DIR='...'
  export JENV_ENV_TYPE='conda|uv'
  export JENV_CONDA_ENV='...'
  export JENV_UV_VENV='...'

Hook behavior:
- source config
- cd into JENV_DIR
- if JENV_ENV_TYPE=conda, activate JENV_CONDA_ENV
- if JENV_ENV_TYPE=uv, source "$JENV_UV_VENV/bin/activate"
- keep output quiet

2. Add env var persistence

Use a separate file:
  ~/.config/jenv/env

Hook should source this file after sourcing config, before or after cd is acceptable, but before env activation may be better for CUDA paths. Recommended:
  source config
  source env
  cd JENV_DIR
  activate conda/uv

Commands:
- jenv env add NAME
  Capture the current value of environment variable NAME and store it in ~/.config/jenv/env as:
    export NAME='value'
  If NAME is not set, print helpful error.
  Reject invalid variable names that are not valid shell identifiers.

- jenv env remove NAME
  Remove NAME from ~/.config/jenv/env.

- jenv env list
  Print captured env vars.

- jenv env clear
  Remove all captured env vars.

3. Add capture-env modes to jenv set

Preferred UX:
- jenv set
  Capture directory + env manager only. Do not capture env vars automatically.

- jenv set --env
  Capture directory + env manager + selected useful env vars using an allowlist.

- jenv set --env-all
  Capture directory + env manager + all env vars except blacklist and secret-looking vars.

- jenv set --uv --env
  uv mode plus safe env capture.

- jenv set --conda test --env
  conda mode plus safe env capture.

Allowlist for --env:
  LD_LIBRARY_PATH
  LIBRARY_PATH
  CPATH
  C_INCLUDE_PATH
  CPLUS_INCLUDE_PATH
  CUDA_HOME
  CUDA_PATH
  CUDNN_HOME
  NCCL_HOME
  PYTHONPATH
  PKG_CONFIG_PATH
  WANDB_PROJECT
  WANDB_ENTITY
  HF_HOME
  HF_DATASETS_CACHE
  TRANSFORMERS_CACHE
  TORCH_HOME
  OMP_NUM_THREADS
  MKL_NUM_THREADS
  OPENBLAS_NUM_THREADS

Do not include PATH in the allowlist by default because it can conflict with conda/venv activation. User can explicitly run:
  jenv env add PATH

Blacklist for --env-all:
  _
  PWD
  OLDPWD
  SHLVL
  SHELL
  HOME
  USER
  LOGNAME
  HOSTNAME
  TERM
  TERMCAP
  COLUMNS
  LINES
  RANDOM
  SECONDS
  UID
  EUID
  PPID
  BASHPID
  BASHOPTS
  BASH_VERSINFO
  BASH_VERSION
  HISTFILE
  HISTSIZE
  HISTCONTROL
  HISTFILESIZE
  PROMPT_COMMAND
  PS1
  PS2
  PS4
  SSH_AUTH_SOCK
  SSH_AGENT_PID
  SSH_CLIENT
  SSH_CONNECTION
  SSH_TTY
  DISPLAY
  WAYLAND_DISPLAY
  XAUTHORITY
  XDG_SESSION_ID
  XDG_RUNTIME_DIR
  DBUS_SESSION_BUS_ADDRESS
  CONDA_PREFIX
  CONDA_PREFIX_1
  CONDA_PREFIX_2
  CONDA_SHLVL
  CONDA_PROMPT_MODIFIER
  CONDA_EXE
  _CE_CONDA
  _CE_M
  VIRTUAL_ENV
  VIRTUAL_ENV_PROMPT

Also skip secret-looking variable names for --env-all:
  variables containing TOKEN, SECRET, PASSWORD, PASS, KEY, CREDENTIAL, COOKIE

If user explicitly runs:
  jenv env add NAME
then allow capturing any non-blacklisted valid shell variable, but warn or reject if it looks secret. Safer default: reject secret-looking names unless a future --allow-secret flag is added.

4. Status output

jenv status should show:
- pinned directory
- env type
- conda env or uv venv
- captured env vars count and names

Example:
  jenv status
  Pinned directory: /home/hpcnc/intern-research
  Env type: conda
  Conda env: test
  Captured env vars:
    LD_LIBRARY_PATH
    CUDA_HOME

5. Purge behavior

jenv purge should remove both:
  ~/.config/jenv/config
  ~/.config/jenv/env

jenv uninstall should remove the bash hook from ~/.bashrc but does not need to remove config unless already designed to do so.

6. Safety
- Use safe single-quote shell escaping for values.
- Validate env var names with regex equivalent:
    ^[A-Za-z_][A-Za-z0-9_]*$
- Do not duplicate env entries.
- Updating env var should replace the old value.
- Keep hook idempotent.
- Keep shell startup quiet.
- Add tests using temporary HOME.
- Test interactive bash startup with captured LD_LIBRARY_PATH and CUDA_HOME.

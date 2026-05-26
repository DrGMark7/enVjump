# jenv

`jenv` pins a startup directory and restores either a Conda environment or a uv virtual environment for future interactive bash shells.

It cannot modify the current parent shell directly. Instead, it writes the desired state to `~/.config/jenv/config`, optionally stores captured environment variables in `~/.config/jenv/env`, and installs a small hook in `~/.bashrc` that runs on future shell startup.

## Build

```sh
make
```

This produces a dependency-free binary with:

```sh
gcc -O2 -Wall -Wextra jenv.c -o jenv
```

## Install

```sh
make install
```

If `~/.local/bin` is not in your `PATH`, add this to `~/.bashrc`:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

## Usage

Pin the current directory and current Conda env:

```sh
jenv set
```

Pin the current directory with an explicit Conda env:

```sh
jenv set test
```

Use explicit Conda mode:

```sh
jenv set --conda
jenv set --conda test
```

Use uv mode. `jenv` will use `$VIRTUAL_ENV` when available, otherwise it will look for `.venv/bin/activate` in the current directory:

```sh
jenv set --uv
```

Capture a safe allowlist of environment variables alongside the pinned directory and env manager:

```sh
jenv set --env
jenv set --conda test --env
jenv set --uv --env
```

Capture most current environment variables while excluding shell state, activation internals, and secret-looking names:

```sh
jenv set --env-all
```

Manage captured environment variables directly:

```sh
jenv env add CUDA_HOME
jenv env remove CUDA_HOME
jenv env list
jenv env clear
```

Install only the shell hook:

```sh
jenv install
```

Show the pinned state:

```sh
jenv status
```

Disable auto-jump for future shells:

```sh
jenv purge
```

Optional cleanup to remove the hook from `~/.bashrc`:

```sh
jenv uninstall
```

## Test

```sh
make test
```

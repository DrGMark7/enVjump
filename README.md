# jenv

`jenv` pins a startup directory and Conda environment for future interactive bash shells.

It cannot modify the current parent shell directly. Instead, it writes the desired state to `~/.config/jenv/config` and installs a small hook in `~/.bashrc` that runs on future shell startup.

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

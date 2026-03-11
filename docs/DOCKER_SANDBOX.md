<!--
SPDX-FileCopyrightText: 2023 Tom Haynes <loghyr@gmail.com>
SPDX-License-Identifier: AGPL-3.0-or-later
-->

# Reffs Docker Sandbox

This document describes the Docker-based sandbox environment for developing and testing the **Reffs** NFS server.

## Overview
The sandbox environment provides a clean, isolated Linux environment (Fedora-based) where the Reffs server can run without interference from the host operating system's NFS services (like kernel `lockd` or system `rpcbind`).

## Requirements
- **Docker**: Must be installed and running.
- **Privileges**: On most Linux systems, you will need `sudo` to run Docker commands unless your user is in the `docker` group.

## Workflow

### 1. Build the Sandbox Image
This step only needs to be performed once or whenever the `Dockerfile` changes.
```bash
make -f Makefile.reffs image
```

### 2. Build the Project in the Sandbox
To ensure the project compiles correctly in the isolated environment without modifying your host system's libraries:
```bash
make -f Makefile.reffs build-in-docker
```
This target mounts your current directory into the container as **read-only**, copies the source to an internal container directory, and runs the build there. This prevents root-owned objects from polluting your host git repository.

### 3. Run the Server in the Sandbox
This command builds the project inside the container and then starts the `reffsd`.
```bash
make -f Makefile.reffs run-image
```
The server is started with the following configuration:
- **Port**: 2049 (mapped to host 2049)
- **RPCPort**: 111 (mapped to host 111)
- **Backend**: POSIX
- **Data Path**: `/tmp/reffs_data` (internal to container)
- **Trace File**: `reffs.trc` (written to project's `logs/` directory)
- **Console Log**: `reffs.console` (written to project's `logs/` directory)

All build artifacts generated during this process stay inside the container and are removed when the container exits (via the `--rm` flag). Logs persist on the host in the `logs/` directory.

### 4. Clean Restart of the Sandbox
If you need to ensure any previously running or hung containers are stopped and removed before starting a fresh server:
```bash
make -f Makefile.reffs test-image
```
This target explicitly runs `stop-image` before `run-image`, resolving issues where `docker-proxy` or previous container instances might interfere with new server runs.

### 5. Debugging a Stuck Server
If the server appears hung or slow, you can dump both kernel-side and user-side stack traces for all `reffsd` threads:
```bash
make -f Makefile.reffs stack
```
This command identifies the `reffsd` process inside the container and outputs:
- **Kernel Stacks**: From `/proc/[pid]/task/[tid]/stack`, showing where threads are blocked in the kernel (e.g., waiting for I/O).
- **User Stacks**: Using `gdb` to provide a full backtrace of the application code.

### 6. Post-Mortem Analysis
The sandbox container uses the `--rm` flag and is removed when it exits. However, logs and traces are **persistent** because they are written to the host's `logs/` directory. This allows you to:
- Inspect the final state of the logs: `ls logs/`
- Check the console output: `cat logs/reffs.console`
- Check the trace file: `tail -f logs/reffs.trc`

### 7. Triaging the Sandbox
If you need to open a shell inside the active sandbox while the server is running:
```bash
sudo docker exec -it reffs-dev-sandbox /bin/bash
```
The image now includes debugging tools like `gdb`, `strace`, `pgrep`, `ps`, and `bash-completion`.

## Useful Commands
The `Makefile.reffs` provides several utility targets for the sandbox workflow:
- `make -f Makefile.reffs help`: Show all available targets.
- `make -f Makefile.reffs test-image`: Stop any existing sandboxes and start a fresh server.
- `make -f Makefile.reffs stack`: Dump stack traces for debugging hung processes.
- `make -f Makefile.reffs reconf`: Force re-generation of the `configure` script inside the sandbox.
- `make -f Makefile.reffs clean`: Remove build artifacts from the `build/` directory.

## Troubleshooting
- **Privileged Mode**: The sandbox runs with `--privileged` to allow FUSE mounts and advanced networking.
- **Port Conflicts**: If port 2049 or 111 is already in use on your host by a system service, you MUST stop those services on the host before starting the sandbox:
  ```bash
  sudo systemctl stop nfs-server rpcbind nfs-lock rpc-statd
  ```
- **Hung Containers or Port Proxies**: If you receive an "address already in use" error when restarting the server, or if `docker-proxy` processes persist after the server shuts down:
  1. Force-remove any remaining sandbox containers:
     ```bash
     sudo docker rm -f $(sudo docker ps -aq --filter ancestor=reffs-dev)
     ```
  2. If ports 111 or 2049 are still bound (check with `ps -ef | grep docker-proxy`), restart the Docker service:
     ```bash
     sudo systemctl restart docker
     ```
- **Shared Build Directory**: The `build/` directory is shared between the host and the container. If you have previously run `./configure` in the root directory, you may encounter an error. Use `make -f Makefile.reffs mrproper` to clean the environment if this happens.

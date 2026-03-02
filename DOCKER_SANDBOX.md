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
sudo make -f Makefile.precommit image
```

### 2. Build the Project in the Sandbox
To ensure the project compiles correctly in the isolated environment without modifying your host system's libraries:
```bash
sudo make -f Makefile.precommit build-in-docker
```
This target mounts your current directory into the container and runs an incremental build using the container's tools (`clang`, `liburing`, etc.).

### 3. Run the Server in the Sandbox
This command ensures the project is built inside the container and then starts the `reffs_nfs3_srv`.
```bash
sudo make -f Makefile.precommit run-image
```
The server is started with the following configuration:
- **Port**: 2049 (mapped to host 2049)
- **RPCPort**: 111 (mapped to host 111)
- **Backend**: POSIX
- **Data Path**: `/tmp/reffs_data` (internal to container)
- **Trace File**: `reffs.trc` (written to host project root)
- **Console Log**: `reffs.console` (written to host project root)

### 4. Running Tests on the Host
While the server is running in the sandbox, you can run tests (like `cthon04`) from your host machine. The client will communicate with the container via the forwarded ports.

**Important**: Use the `nolock` option to avoid dependencies on host RPC services (like `rpc-statd`) that might conflict with the sandbox.

```bash
sudo mount -o tcp,mountproto=tcp,vers=3,nolock 127.0.0.1:/ /mnt/reffs
# Then run tests
sudo ./cthon04/do_cthon.sh 127.0.0.1:/ /mnt/cthon04
```

### 5. Triaging the Sandbox
If you need to open a shell inside the active sandbox while the server is running:
```bash
sudo docker exec -it $(sudo docker ps -q --filter ancestor=reffs-dev) /bin/bash
```

## Useful Commands
The `Makefile.precommit` provides several utility targets for the sandbox workflow:
- `make -f Makefile.precommit help`: Show all available targets.
- `make -f Makefile.precommit reconf`: Force re-generation of the `configure` script inside the sandbox.
- `make -f Makefile.precommit clean`: Remove build artifacts from the `build/` directory.

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
- **Shared Build Directory**: The `build/` directory is shared between the host and the container. If you have previously run `./configure` in the root directory, you may encounter an error. Use `make -f Makefile.precommit mrproper` to clean the environment if this happens.

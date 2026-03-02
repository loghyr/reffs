# Reffs Docker Sandbox

This document describes the Docker-based sandbox environment for developing and testing the **Reffs** NFS server.

## Overview
The sandbox environment provides a clean, isolated Linux environment (Fedora-based) where the Reffs server can run without interference from the host operating system's NFS services (like kernel `lockd` or system `rpcbind`).

### Benefits
- **Port Isolation**: The container has its own private RPC Portmapper. System services on the host cannot "hijack" or overwrite Reffs' RPC registrations.
- **Dependency Management**: All build and runtime dependencies are pre-installed in the image.
- **Consistent Triage**: Traces and console output are mirrored to the host filesystem for easy analysis.

## Workflow

### 1. Build the Sandbox Image
This step only needs to be performed once or whenever the `Dockerfile` changes.
```bash
make -f Makefile.precommit image
```

### 2. Run the Server in the Sandbox
This command builds the project inside the container and then starts the `reffs_nfs3_srv`.
```bash
make -f Makefile.precommit run-image
```
The server is started with the following configuration:
- **Port**: 2049 (mapped to host 2049)
- **RPCPort**: 111 (mapped to host 111)
- **Backend**: POSIX
- **Data Path**: `/tmp/reffs_data` (internal to container)
- **Trace File**: `reffs.trc` (written to host project root)
- **Console Log**: `reffs.console` (written to host project root)

### 3. Running Tests on the Host
While the server is running in the sandbox, you can run tests (like `cthon04`) from your host machine. The client will communicate with the container via the forwarded ports.
```bash
sudo ./cthon04/do_cthon.sh 127.0.0.1:/ /mnt/cthon04
```

### 4. Triaging the Sandbox
If you need to open a shell inside the active sandbox while the server is running:
```bash
docker exec -it $(docker ps -q --filter ancestor=reffs-dev) /bin/bash
```

## Troubleshooting
- **Privileged Mode**: The sandbox runs with `--privileged` to allow FUSE mounts and advanced networking. Ensure your Docker daemon allows privileged containers.
- **Port Conflicts**: If port 2049 or 111 is already in use on your host by a system service, you may need to stop those services on the host before starting the sandbox.

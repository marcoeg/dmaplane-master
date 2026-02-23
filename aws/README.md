# NUMA Validation on EC2 Dual-Socket Hardware

Phase 5 NUMA tests run on a single-node dev machine but only prove the code compiles and the API works. The cross-node bandwidth penalty — the entire point of NUMA awareness — requires multi-socket hardware. This stack provisions a dual-socket c5.metal instance, builds the driver, runs all tests, and captures the results.

## Prerequisites

- AWS CLI configured with credentials that can create EC2 instances, security groups, and Elastic IPs
- An existing VPC with a subnet that has internet access (internet gateway route)
- An existing EC2 key pair for SSH access
- The dmaplane repo must be publicly accessible at https://github.com/marcoeg/dmaplane-master.git

## Deploy

```bash
aws cloudformation create-stack \
  --stack-name dmaplane-numa-test \
  --template-body file://dmaplane-numa-test.yaml \
  --parameters \
    ParameterKey=SubnetId,ParameterValue=<your-public-subnet-id> \
    ParameterKey=KeyPairName,ParameterValue=<your-key-pair-name>
```

The c5.metal instance takes 5–8 minutes to provision (bare metal, no hypervisor). UserData installs build dependencies, clones the repo, builds the driver and tests, sets up Soft-RoCE, and runs all five test phases. Total setup time: ~15 minutes.

### Finding a subnet

```bash
aws ec2 describe-subnets \
  --filters "Name=vpc-id,Values=<your-vpc-id>" \
  --query "Subnets[].[SubnetId,AvailabilityZone,MapPublicIpOnLaunch]" \
  --output table
```

## Connect

Get the Elastic IP from the stack outputs:

```bash
aws cloudformation describe-stacks \
  --stack-name dmaplane-numa-test \
  --query "Stacks[0].Outputs[?OutputKey=='PublicIP'].OutputValue" \
  --output text
```

SSH in:

```bash
ssh -i <key>.pem ubuntu@<elastic-ip>
```

Check progress:

```bash
tail -f /var/log/dmaplane-setup.log        # build and test progress
ls ~/TESTS_DONE 2>/dev/null && echo "Done"  # completion flag
```

## Run Tests Manually

If UserData completed successfully, results are already in `~/dmaplane/results/`. To re-run or run interactively:

```bash
# Load Soft-RoCE (if not already loaded)
PRIMARY_IF=$(ip -o link show | awk -F': ' '/state UP/ && !/lo/ {print $2; exit}')
sudo modprobe rdma_rxe
sudo rdma link add rxe_${PRIMARY_IF} type rxe netdev ${PRIMARY_IF} 2>/dev/null || true

# Load driver
cd ~/dmaplane/driver
sudo insmod dmaplane.ko

# Run all phases
cd ~/dmaplane/tests
sudo ./test_phase1_driver
sudo ./test_phase2_dma
sudo ./test_phase3_dmabuf
sudo ./test_phase4_rdma
sudo ./test_phase5_numa

# Unload
sudo rmmod dmaplane
```

### Running the 64 MB benchmark

The default benchmark uses 1 MB buffers. On the c5.metal (36 MB L3 per socket), 1 MB fits entirely in cache and masks the NUMA penalty. To measure actual DRAM bandwidth, increase the buffer to 64 MB:

```bash
cd ~/dmaplane/tests
sed -i 's/buffer_size = 1 << 20/buffer_size = 64 << 20/' test_phase5_numa.c
make
sudo ./test_phase5_numa
```

## Expected Results

### Instance topology (c5.metal)

```
Dual-socket Xeon Platinum 8124M
96 CPUs (48 per socket, 24 cores × 2 threads)
~189 GB RAM (~94 GB per node)
36 MB L3 per socket
SLIT distance: 10 (local), 21 (remote)
```

### Bandwidth matrix — 1 MB buffers (L3-resident)

```
  cpu\buf | node0  | node1  |
  node0   |  8015  |  8983  |
  node1   |  8945  |  9637  |
```

No penalty visible. Both source and destination fit in L3 cache after the memset pre-touch. The benchmark measures cache-to-cache transfer, not DRAM access.

### Bandwidth matrix — 64 MB buffers (exceeds L3)

```
  cpu\buf | node0  | node1  |
  node0   |  6778  |  5577  |
  node1   |  5013  |  6095  |
```

18% penalty both directions. Local access averages ~6.4 GB/s, remote ~5.3 GB/s. This is the real QPI/UPI cost that production workloads hit with gradient buffers and KV-cache blocks.

### Why the two numbers matter

| Buffer | Local | Remote | Penalty | What you're measuring |
|--------|-------|--------|---------|----------------------|
| 1 MB   | ~8.8 GB/s | ~8.9 GB/s | 0% | L3 cache bandwidth |
| 64 MB  | ~6.4 GB/s | ~5.3 GB/s | 18% | DRAM + QPI interconnect |

The 18% penalty on Skylake-SP is lower than older Broadwell Xeons (40–50%) due to a wider UPI mesh. It still compounds across every all-reduce iteration in distributed training.

## Retrieve Results

From your local machine:

```bash
scp -i <key>.pem ubuntu@<elastic-ip>:~/dmaplane/results/* .
```

Files:
- `test_phase5.txt` — NUMA test output (topology, bandwidth matrix, stats)
- `test_phase1.txt` through `test_phase4.txt` — regression test outputs
- `topology.txt` — `numactl --hardware` and `lstopo` output
- `dmesg.txt` — kernel messages filtered for dmaplane/NUMA/warnings
- `double_cycle.txt` — load/test/unload/reload/test/unload cycle

## Tear Down

```bash
aws cloudformation delete-stack --stack-name dmaplane-numa-test
```

c5.metal costs ~$4.60/hr on-demand. A full test run takes ~20 minutes. Delete promptly.

## Known Issues

**`rdma_rxe` not found.** AWS kernels ship a stripped module set. The template installs `linux-modules-extra-$(uname -r)` to provide RDMA modules. If this fails, install manually:

```bash
sudo apt-get install -y linux-modules-extra-$(uname -r)
sudo modprobe rdma_rxe
```

**Subnet has no public IP.** The template attaches an Elastic IP, so `MapPublicIpOnLaunch` is not required. However, the subnet must have a route to an internet gateway for `apt-get` and `git clone` to work.

**AMI region mapping.** The template includes AMI IDs for us-east-1, us-east-2, us-west-1, us-west-2, and eu-west-1 (Ubuntu 24.04 LTS). For other regions, find the AMI:

```bash
aws ec2 describe-images \
  --owners 099720109477 \
  --filters "Name=name,Values=ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*" \
  --query "Images | sort_by(@, &CreationDate) | [-1].ImageId" \
  --output text
```

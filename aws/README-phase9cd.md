# Phase 9C/D — Two-Instance Disaggregated Inference Demo

2× g5.xlarge (A10G) from the Phase 9B AMI. Everything pre-built — instances boot ready to run peer mode transfers and disaggregated inference in under 5 minutes.

## What It Does

The CloudFormation stack launches two g5.xlarge instances in a **cluster placement group** (same rack, lowest latency). UserData on each instance:

1. Waits for GPU driver
2. Configures soft-RoCE (rxe) on the primary interface
3. Loads dmaplane.ko (rebuilds if kernel changed)
4. Runs a loopback smoke test
5. Prints usage instructions with the private IP

No auto-stop — you control the session. Tests are run manually via SSH.

## Prerequisites

- Phase 9B AMI: `ami-0960d74ca58008230` (us-west-2, pre-built)
- Public subnet in `vpc-0eba669e962960152` with internet gateway route
- EC2 key pair for SSH access

## Deploy

```bash
aws cloudformation create-stack \
  --stack-name dmaplane-phase9cd \
  --region us-west-2 \
  --template-body file://dmaplane-phase9cd.yaml \
  --parameters \
    ParameterKey=SubnetId,ParameterValue=subnet-0fe4b39afc43c4fb2 \
    ParameterKey=KeyPairName,ParameterValue=<your-key-pair> \
    ParameterKey=AmiId,ParameterValue=ami-0960d74ca58008230
```

Wait for stack creation (~3 minutes):

```bash
aws cloudformation wait stack-create-complete \
  --stack-name dmaplane-phase9cd --region us-west-2
```

## Connect

```bash
# Get IPs
aws cloudformation describe-stacks \
  --stack-name dmaplane-phase9cd --region us-west-2 \
  --query "Stacks[0].Outputs" --output table

# SSH
ssh -i <key>.pem ubuntu@<PrefillPublicIP>   # sender (Machine A)
ssh -i <key>.pem ubuntu@<DecodePublicIP>     # receiver (Machine B)
```

Wait for ready on each instance:

```bash
while [ ! -f ~/READY ]; do sleep 5; echo "waiting..."; done
cat /var/log/dmaplane-setup.log
```

## Phase 9C Test Plan

All commands run from `~/dmaplane/examples/kvcache/`. Always start the **receiver first**.

### 1. Network baseline (from prefill)

```bash
ping -c 5 <DecodePrivateIP>
# On decode: iperf3 -s
# On prefill: iperf3 -c <DecodePrivateIP>
```

### 2. Host-backed peer transfer (small)

```bash
# decode:
sudo ./kvcache_receiver --port 9876 --verify

# prefill:
sudo ./kvcache_sender --peer <DecodePrivateIP> --port 9876 --verify \
  --layers 4 --chunks-per-layer 2
```

### 3. Full 128-chunk transfer

```bash
# decode:
sudo ./kvcache_receiver --port 9876 --verify

# prefill:
sudo ./kvcache_sender --peer <DecodePrivateIP> --port 9876 --verify \
  --layers 32 --chunks-per-layer 4
```

### 4. GPU-backed transfer

```bash
# decode:
sudo ./kvcache_receiver --port 9876 --gpu --verify

# prefill:
sudo ./kvcache_sender --peer <DecodePrivateIP> --port 9876 --gpu --verify
```

### 5. Credit pressure (window=2)

```bash
# decode:
sudo ./kvcache_receiver --port 9876 --verify

# prefill:
sudo ./kvcache_sender --peer <DecodePrivateIP> --port 9876 --verify \
  --credit-window 2 --layers 8
```

### 6. Chunk size sweep

```bash
for sz in 262144 1048576 4194304; do
  # decode: sudo ./kvcache_receiver --port 9876 --verify
  # prefill:
  sudo ./kvcache_sender --peer <DecodePrivateIP> --port 9876 --verify \
    --chunk-size $sz --layers 16
done
```

### 7. Stress: 10× consecutive

```bash
for i in $(seq 1 10); do
  # decode: sudo ./kvcache_receiver --port 9876 --verify
  # prefill:
  sudo ./kvcache_sender --peer <DecodePrivateIP> --port 9876 --verify
done
```

## Phase 9D: Disaggregated Inference

After 9C validates the RDMA transport, 9D runs real LLM inference across the two machines. Machine A prefills, ships KVCache via WRITEIMM, Machine B decodes.

Both scripts are in `~/dmaplane/examples/kvcache/`. Always start the **receiver (decode) first**.

### 8. TinyLlama end-to-end (the money shot)

```bash
# decode (Machine B) — start first:
sudo python3 decode_server.py --peer <PrefillPrivateIP> --port 9876

# prefill (Machine A):
sudo python3 prefill_server.py --port 9876
```

Expected: decode prints streaming tokens for 3 default prompts. Verify output is coherent English, not garbage.

### 9. Verify token-exact match

On either machine, run single-machine greedy generation for the same prompts and compare output:

```python
python3 -c "
from transformers import AutoModelForCausalLM, AutoTokenizer
import torch
m = AutoModelForCausalLM.from_pretrained('TinyLlama/TinyLlama-1.1B-Chat-v1.0',
    torch_dtype=torch.float16, device_map='cuda')
t = AutoTokenizer.from_pretrained('TinyLlama/TinyLlama-1.1B-Chat-v1.0')
for p in ['Explain the theory of relativity in simple terms.']:
    ids = t(p, return_tensors='pt').to('cuda')
    out = m.generate(**ids, max_new_tokens=100, do_sample=False)
    print(t.decode(out[0], skip_special_tokens=True))
"
```

Disaggregated output should match token-for-token (greedy deterministic).

### 10. Custom chunk size + credit window

```bash
# decode:
sudo python3 decode_server.py --peer <PrefillPrivateIP> --port 9876

# prefill (4MB chunks, wider credit window):
sudo python3 prefill_server.py --port 9876 --chunk-size 4194304 --credit-window 32
```

### 11. Collect timing data for blog

Save the terminal output from both sides. Key numbers to capture:

- Prefill compute time (ms)
- KVCache serialize time (ms)
- KVCache transfer time (ms) + throughput (MB/s)
- KVCache reconstruct time (ms)
- Time-to-first-token (ms)
- Decode tok/s
- Credit stalls

## Cost

- 2× g5.xlarge on-demand: ~$2.01/hr total
- 2× g5.xlarge spot: ~$0.70–1.00/hr total
- Typical session (3 hours): **~$2.10–6.00**
- Cluster placement group: no extra cost

## Tear Down

```bash
aws cloudformation delete-stack --stack-name dmaplane-phase9cd --region us-west-2
```

**Important:** No auto-stop. Instances run until you delete the stack or stop them manually.

```bash
# Stop both (preserves EBS, stops billing compute):
aws ec2 stop-instances --region us-west-2 --instance-ids <PrefillId> <DecodeId>

# Restart later:
aws ec2 start-instances --region us-west-2 --instance-ids <PrefillId> <DecodeId>
```

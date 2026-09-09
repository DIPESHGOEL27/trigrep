#!/bin/bash
# EC2 user-data for the Task 11 scaling run.
#
# Proves the resume claim that peak RSS is bounded by (threads x run buffer +
# offset table) and is INDEPENDENT of corpus size: the identical binary indexes
# Linux alone (~1.2 GB) and then Linux+LLVM+PostgreSQL+CPython (~5 GB), and the
# two peak-RSS figures should be within noise of each other.
#
# Observability: the full log is PUT to a presigned S3 URL after every stage
# and again from the exit trap, so a failure anywhere still leaves evidence.
# The instance then shuts down; launched with
# --instance-initiated-shutdown-behavior terminate so it also terminates.
#
# __SRC_URL__ / __PUT_URL__ are substituted at launch with presigned S3 URLs.
LOG=/var/log/trigrep.log
: > "$LOG"
exec > >(tee -a "$LOG") 2>&1
set -ux
PUT_URL='__PUT_URL__'
upload() { curl -sS -X PUT -T "$LOG" "$PUT_URL" -o /dev/null -w "upload(%{http_code}) " || true; echo; }
trap 'rc=$?; echo "=== bootstrap exiting rc=$rc at $(date -u +%FT%TZ) ==="; upload; sleep 5; shutdown -h now' EXIT

echo "=== trigrep scaling run start $(date -u +%FT%TZ) ==="
echo "--- host ---"; nproc; free -g | head -2; df -h / | tail -1; uname -r
echo "--- network ---"; ip -4 route | head -3; curl -sS -m 10 -o /dev/null -w "egress http_code=%{http_code}\n" https://cdn.kernel.org/ || echo "egress FAILED"
upload

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq g++ make git curl xz-utils time >/dev/null
g++ --version | head -1; upload

mkdir -p /work/corpus && cd /work
curl -sSL -o src.tgz "__SRC_URL__" && ls -l src.tgz && tar xzf src.tgz && make -s && ls build/
upload

cd /work/corpus
echo "=== fetch corpora $(date -u +%T) ==="
curl -sSL -o l.tar.xz https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.tar.xz \
  && tar xf l.tar.xz && rm l.tar.xz && echo "linux ok"
git clone -q --depth 1 https://github.com/llvm/llvm-project.git   && rm -rf llvm-project/.git && echo "llvm ok"
git clone -q --depth 1 https://github.com/postgres/postgres.git    && rm -rf postgres/.git     && echo "postgres ok"
git clone -q --depth 1 https://github.com/python/cpython.git       && rm -rf cpython/.git      && echo "cpython ok"
du -sh /work/corpus/* /work/corpus; df -h / | tail -1
upload
cd /work

echo "=== RUN 1: Linux only $(date -u +%T) ==="
./build/count_trigrams corpus/linux-6.6
/usr/bin/time -f "time_wall=%e time_maxrss_kb=%M" ./build/trigrep_index corpus/linux-6.6 idx_linux
du -sh idx_linux
upload

echo "=== RUN 2: Linux + LLVM + PostgreSQL + CPython, IDENTICAL binary $(date -u +%T) ==="
./build/count_trigrams corpus
/usr/bin/time -f "time_wall=%e time_maxrss_kb=%M" ./build/trigrep_index corpus idx_all
du -sh idx_all
upload

echo "=== sanity: same query, both indexes ==="
for IX in idx_linux idx_all; do
  printf "%-10s " "$IX"
  ./build/trigrep_search "$IX" 'kmalloc\(.*GFP_ATOMIC' --stats 2>&1 >/dev/null \
    | grep -oE "corpus_files [0-9]+|candidates [0-9]+|hits [0-9]+|total_ms [0-9.]+" | tr '\n' ' '; echo
done

echo "=== RESULT: compare peak_rss_mb across RUN 1 and RUN 2 above ==="
echo "=== trigrep scaling run end $(date -u +%FT%TZ) ==="

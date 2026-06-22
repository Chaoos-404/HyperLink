#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  scripts/deploy_server.sh --host <usb-ip> [options]

Options:
  --host <ip-or-name>             Mac mini IP on the USB4/Thunderbolt network.
  --user <ssh-user>               SSH user on the Mac mini. Default: user
  --remote-dir <path>             Remote project directory. Default: ~/Hyperlink
  --port <port>                   First benchmark server port. Default: 47777
  --parallel <count>              Number of server streams. Default: 8
  --socket-buffer-bytes <bytes>   Socket send/receive buffer size. Default: 8388608
  --build-preset <preset>         CMake preset to build remotely. Default: release
  --no-start                      Sync and build, but do not start the server.
  --foreground                    Start server in the SSH session instead of backgrounding it.
  -h, --help                      Show this help.

Examples:
  scripts/deploy_server.sh --host 169.254.119.3 --user user

  scripts/deploy_server.sh --host 169.254.119.3 --user user \
    --parallel 8 --socket-buffer-bytes 8388608
USAGE
}

ssh_user="user"
ssh_host=""
remote_dir="~/Hyperlink"
port="47777"
parallel="8"
socket_buffer_bytes="8388608"
build_preset="release"
start_server="yes"
foreground="no"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)
      ssh_host="${2:?--host requires a value}"
      shift 2
      ;;
    --user)
      ssh_user="${2:?--user requires a value}"
      shift 2
      ;;
    --remote-dir)
      remote_dir="${2:?--remote-dir requires a value}"
      shift 2
      ;;
    --port)
      port="${2:?--port requires a value}"
      shift 2
      ;;
    --parallel)
      parallel="${2:?--parallel requires a value}"
      shift 2
      ;;
    --socket-buffer-bytes)
      socket_buffer_bytes="${2:?--socket-buffer-bytes requires a value}"
      shift 2
      ;;
    --build-preset)
      build_preset="${2:?--build-preset requires a value}"
      shift 2
      ;;
    --no-start)
      start_server="no"
      shift
      ;;
    --foreground)
      foreground="yes"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "$ssh_host" ]]; then
  echo "--host is required" >&2
  usage >&2
  exit 1
fi

case "$port" in
  ''|*[!0-9]*)
    echo "--port must be a positive integer" >&2
    exit 1
    ;;
esac

case "$parallel" in
  ''|*[!0-9]*)
    echo "--parallel must be a positive integer" >&2
    exit 1
    ;;
esac

if (( port < 1 || port > 65535 )); then
  echo "--port must be between 1 and 65535" >&2
  exit 1
fi

if (( parallel < 1 || port + parallel - 1 > 65535 )); then
  echo "--parallel must be at least 1 and port + parallel - 1 must not exceed 65535" >&2
  exit 1
fi

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ssh_target="${ssh_user}@${ssh_host}"
remote_shell_dir="$remote_dir"

echo "Deploying Hyperlink to ${ssh_target}:${remote_dir}"
echo "Using server ports ${port}..$((port + parallel - 1))"

ssh "$ssh_target" "mkdir -p ${remote_shell_dir}"

rsync -az --delete \
  --exclude '.git/' \
  --exclude '.cache/' \
  --exclude '.DS_Store' \
  --exclude 'build/' \
  --exclude 'cmake-build-*/' \
  --exclude 'Hyperlink-dev-source.tar.gz' \
  "${source_dir}/" "${ssh_target}:${remote_dir}/"

remote_command=$(cat <<REMOTE
set -euo pipefail
export PATH="/opt/homebrew/bin:/usr/local/bin:\$PATH"
if [[ -x /opt/homebrew/bin/brew ]]; then
  eval "\$(/opt/homebrew/bin/brew shellenv)"
elif [[ -x /usr/local/bin/brew ]]; then
  eval "\$(/usr/local/bin/brew shellenv)"
fi
command -v cmake >/dev/null || {
  echo "cmake was not found in the remote SSH PATH. Install it with: brew install cmake ninja pkg-config libusb" >&2
  exit 127
}
cd ${remote_shell_dir}
cmake --preset ${build_preset}
cmake --build --preset ${build_preset}
pkill -f '[h]yperlink_bench --server' 2>/dev/null || true
REMOTE
)

if [[ "$start_server" == "yes" ]]; then
  if [[ "$foreground" == "yes" ]]; then
    remote_command+=$(cat <<REMOTE

exec ./build/${build_preset}/hyperlink_bench --server --host 0.0.0.0 --port ${port} --parallel ${parallel} --socket-buffer-bytes ${socket_buffer_bytes}
REMOTE
)
  else
    remote_command+=$(cat <<REMOTE

nohup ./build/${build_preset}/hyperlink_bench --server --host 0.0.0.0 --port ${port} --parallel ${parallel} --socket-buffer-bytes ${socket_buffer_bytes} > hyperlink_server.log 2>&1 &
sleep 1
tail -n 20 hyperlink_server.log || true
REMOTE
)
  fi
fi

ssh "$ssh_target" "$remote_command"

if [[ "$start_server" == "yes" && "$foreground" == "no" ]]; then
  cat <<EOF

Server deployed and started on ${ssh_host}:${port}..$((port + parallel - 1)).
Remote log:
  ssh ${ssh_target} 'tail -f ${remote_dir}/hyperlink_server.log'

Client command:
  ./build/${build_preset}/hyperlink_bench --client --host ${ssh_host} --port ${port} --seconds 10 --chunk-bytes 4194304 --parallel ${parallel} --socket-buffer-bytes ${socket_buffer_bytes}
EOF
fi

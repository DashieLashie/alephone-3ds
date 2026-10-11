#!/usr/bin/env bash
# Read and write a 3DS SD card over ftpd (Homebrew Launcher app, port 5000),
# with curl only (macOS has no ftp client). Paths are SD-card paths: "/" is
# sdmc:/.
#
#   scripts/3ds/sd-ftp.sh IP ls   [DIR]            list a directory
#   scripts/3ds/sd-ftp.sh IP get  SDPATH [LOCAL]   download a file (LOCAL: a file or a directory)
#   scripts/3ds/sd-ftp.sh IP put  LOCAL SDDIR      upload a file into SDDIR (created if missing)
#   scripts/3ds/sd-ftp.sh IP putdir LOCALDIR SDDIR upload a directory tree as SDDIR
#   scripts/3ds/sd-ftp.sh IP touch SDPATH          create an empty file (flag files)
#   scripts/3ds/sd-ftp.sh IP rm   SDPATH           delete a file
#   scripts/3ds/sd-ftp.sh IP mv   SDPATH NEWPATH   rename a file
#   scripts/3ds/sd-ftp.sh IP size SDPATH           print a file's size in bytes
#
# e.g. scripts/3ds/sd-ftp.sh 192.168.1.50 get /3ds/alephone/log.txt build-3ds/hw/
#
# Environment: FTP_PORT (default 5000).
set -euo pipefail

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
[ $# -ge 2 ] || usage 1
case "$1" in -h|--help) usage ;; esac

ip=$1 cmd=$2; shift 2
base="ftp://$ip:${FTP_PORT:-5000}"
# Apple's curl: macOS's Local Network privacy blocks other builds (e.g.
# Anaconda's, often first in PATH) from LAN addresses: "Failed to connect
# ... after 0 ms".
curl_ftp=(/usr/bin/curl --silent --show-error --fail --connect-timeout 10)

# Percent-encode an SD path for a URL, keeping the slashes.
enc() { python3 -I -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$1"; }

# DIR must end in exactly one slash for curl to treat it as a directory.
dir_url() { local d=${1%/}; echo "$base$(enc "$d")/"; }

case "$cmd" in
ls)
	"${curl_ftp[@]}" "$(dir_url "${1:-/}")"
	;;
get)
	src=$1 dst=${2:-.}
	[ -d "$dst" ] && dst="${dst%/}/$(basename "$src")"
	"${curl_ftp[@]}" -o "$dst" "$base$(enc "$src")"
	echo "$dst ($(wc -c < "$dst" | tr -d ' ') bytes)"
	;;
put)
	"${curl_ftp[@]}" --ftp-create-dirs -T "$1" "$(dir_url "$2")"
	echo "uploaded $1 -> ${2%/}/$(basename "$1")"
	;;
putdir)
	src=${1%/} dst=${2%/}
	n=0 bytes=0 start=$(date +%s)
	while IFS= read -r -d '' f; do
		rel=${f#"$src"/}
		reldir=$(dirname "$rel"); [ "$reldir" = . ] && reldir= || reldir=/$reldir
		"${curl_ftp[@]}" --ftp-create-dirs -T "$f" "$(dir_url "$dst$reldir")"
		n=$((n + 1)) bytes=$((bytes + $(wc -c < "$f")))
		printf '\r%d files, %d KiB' "$n" $((bytes / 1024))
	done < <(find "$src" -name .git -prune -o -type f ! -name .DS_Store -print0 | sort -z)
	secs=$(( $(date +%s) - start )); [ "$secs" -gt 0 ] || secs=1
	printf '\nuploaded %d files, %d bytes in %d s (%d KiB/s) to %s\n' \
		"$n" "$bytes" "$secs" $((bytes / 1024 / secs)) "$dst"
	;;
touch)
	"${curl_ftp[@]}" --ftp-create-dirs -T /dev/null "$base$(enc "$1")"
	echo "created $1"
	;;
rm)
	"${curl_ftp[@]}" -o /dev/null -Q "DELE $1" "$base/"
	echo "deleted $1"
	;;
mv)
	"${curl_ftp[@]}" -o /dev/null -Q "RNFR $1" -Q "RNTO $2" "$base/"
	echo "renamed $1 -> $2"
	;;
size)
	"${curl_ftp[@]}" -I "$base$(enc "$1")" | sed -n 's/^Content-Length: *//p' | tr -d '\r'
	;;
*)
	usage 1
	;;
esac

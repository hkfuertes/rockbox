#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME=$(basename "$0")
DEFAULT_CFG_REMOTE="/sdcard/.rockbox/audiobookshelf.cfg"
DEFAULT_CACERT_REMOTE="/data/data/cacert.pem"
DEVICE_GOCURL="/data/data/gocurl"
REDACTED_TOKEN="<redacted-token>"
REDACTED_AUTH="<redacted>"

CFG_PATH=""
CACERT_PATH=""
HTTP_CLIENT="auto"
GOCURL_PATH=""
PULL_CFG=0
PULL_CACERT=0
RUN_DEVICE_CHECK=0
ADB_SERIAL=""
WORKDIR=""
KEEP_WORKDIR=0
HOST_CONNECT_TIMEOUT=10

SERVER_URL=""
TOKEN=""
LIBRARY_ID=""
DOWNLOAD_DIR=""
PAGE_SIZE="20"
FIRST_ITEM_ID=""

HOST_API_OK=0
DEVICE_OK=0
DEVICE_SKIPPED=0

usage() {
    cat <<EOF
Usage: $SCRIPT_NAME [options]

Audiobookshelf API smoke harness for host-side config/API validation and
optional Y1 /data/data/gocurl validation over adb.

Options:
  --cfg PATH              Use local audiobookshelf.cfg
  --cacert PATH           Use local CA bundle/cert file
  --pull-cfg              Pull $DEFAULT_CFG_REMOTE from adb into a temp dir
  --pull-cacert           Pull $DEFAULT_CACERT_REMOTE from adb into a temp dir
  --device-check          Also exercise $DEVICE_GOCURL on the device via adb
  --http-client NAME      auto|curl|gocurl (default: auto)
  --gocurl PATH           Host gocurl binary path when --http-client=gocurl
  --adb-serial SERIAL     Pass adb -s SERIAL
  --keep-workdir          Keep temp files for inspection
  --help                  Show this help

Examples:
  $SCRIPT_NAME --pull-cfg --pull-cacert
  $SCRIPT_NAME --cfg /tmp/audiobookshelf.cfg --pull-cacert --device-check
  $SCRIPT_NAME --cfg /tmp/audiobookshelf.cfg --cacert /tmp/cacert.pem --device-check
EOF
}

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

have_cmd() {
    command -v "$1" >/dev/null 2>&1
}

adb_cmd() {
    if [[ -n "$ADB_SERIAL" ]]; then
        adb -s "$ADB_SERIAL" "$@"
    else
        adb "$@"
    fi
}

cleanup() {
    if [[ -n "$WORKDIR" && -d "$WORKDIR" && "$KEEP_WORKDIR" -eq 0 ]]; then
        rm -rf "$WORKDIR"
    fi
}
trap cleanup EXIT

make_workdir() {
    if [[ -z "$WORKDIR" ]]; then
        WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/abs-smoke.XXXXXX")
    fi
}

redact_stream() {
    TOKEN_ENV="$TOKEN" python3 -c '
import os, re, sys
text = sys.stdin.read()
token = os.environ.get("TOKEN_ENV", "")
text = re.sub(r"(?im)^(Authorization\s*:\s*Bearer)\s+.+$", r"\1 <redacted>", text)
text = re.sub(r"(?im)^(Authorization\s*:\s*Token)\s+.+$", r"\1 <redacted>", text)
if token:
    text = text.replace(token, "<redacted-token>")
sys.stdout.write(text)
'
}

redact_text() {
    printf '%s' "$1" | redact_stream
}

print_kv() {
    printf '  %-18s %s\n' "$1" "$2"
}

shell_quote_sq() {
    python3 -c 'import shlex, sys; print(shlex.quote(sys.argv[1]))' "$1"
}

parse_cfg() {
    local cfg=$1
    python3 - "$cfg" <<'PY'
import sys
path = sys.argv[1]
vals = {
    'server_url': '',
    'token': '',
    'library_id': '',
    'download_dir': '',
    'page_size': '20',
}
with open(path, 'r', encoding='utf-8') as fh:
    for raw in fh:
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if ':' not in line:
            continue
        key, value = line.split(':', 1)
        key = key.strip()
        value = value.strip()
        if key in vals:
            vals[key] = value
for k in ('server_url','token','library_id','download_dir','page_size'):
    print(f"{k}={vals[k]}")
PY
}

sanitize_cfg() {
    SERVER_URL=${SERVER_URL%/}
    if [[ -z "$SERVER_URL" ]]; then
        fail "missing server_url in cfg"
    fi
    if [[ -z "$TOKEN" ]]; then
        fail "missing token in cfg"
    fi
    if [[ -z "$LIBRARY_ID" ]]; then
        fail "missing library_id in cfg"
    fi
    if ! [[ "$PAGE_SIZE" =~ ^[0-9]+$ ]]; then
        PAGE_SIZE=20
    fi
    if (( PAGE_SIZE < 1 )); then
        PAGE_SIZE=20
    elif (( PAGE_SIZE > 50 )); then
        PAGE_SIZE=50
    fi
}

load_cfg() {
    local line key value
    while IFS= read -r line; do
        key=${line%%=*}
        value=${line#*=}
        case "$key" in
            server_url) SERVER_URL=$value ;;
            token) TOKEN=$value ;;
            library_id) LIBRARY_ID=$value ;;
            download_dir) DOWNLOAD_DIR=$value ;;
            page_size) PAGE_SIZE=$value ;;
        esac
    done < <(parse_cfg "$CFG_PATH")
    sanitize_cfg
}

pull_cfg() {
    make_workdir
    CFG_PATH="$WORKDIR/audiobookshelf.cfg"
    adb_cmd pull "$DEFAULT_CFG_REMOTE" "$CFG_PATH" >/dev/null
}

pull_cacert() {
    make_workdir
    CACERT_PATH="$WORKDIR/cacert.pem"
    adb_cmd exec-out su -c "cat '$DEFAULT_CACERT_REMOTE'" >"$CACERT_PATH"
    [[ -s "$CACERT_PATH" ]] || fail "pulled cacert is empty"
}

resolve_http_client() {
    case "$HTTP_CLIENT" in
        auto)
            if have_cmd curl; then
                HTTP_CLIENT="curl"
            elif [[ -n "$GOCURL_PATH" || $(have_cmd gocurl; echo $?) -eq 0 ]]; then
                HTTP_CLIENT="gocurl"
            else
                fail "no supported host HTTP client found (need curl or gocurl)"
            fi
            ;;
        curl)
            have_cmd curl || fail "curl not found"
            ;;
        gocurl)
            if [[ -z "$GOCURL_PATH" ]]; then
                GOCURL_PATH=$(command -v gocurl || true)
            fi
            [[ -n "$GOCURL_PATH" && -x "$GOCURL_PATH" ]] || fail "gocurl not found or not executable"
            ;;
        *)
            fail "unsupported --http-client '$HTTP_CLIENT'"
            ;;
    esac

    if [[ "$HTTP_CLIENT" == "gocurl" && -z "$GOCURL_PATH" ]]; then
        GOCURL_PATH=$(command -v gocurl)
    fi
}

host_request() {
    local url=$1
    local body_file=$2
    local meta_file=$3
    local stderr_file=$4

    if [[ "$HTTP_CLIENT" == "curl" ]]; then
        local status rc
        rc=0
        if [[ -n "$CACERT_PATH" ]]; then
            status=$(curl --cacert "$CACERT_PATH" --connect-timeout "$HOST_CONNECT_TIMEOUT" -L -sS \
                -H "Authorization: Bearer $TOKEN" \
                -H "Accept: application/json" \
                -o "$body_file" -D "$meta_file" -w '%{http_code}' "$url" 2>"$stderr_file") || rc=$?
        else
            status=$(curl --connect-timeout "$HOST_CONNECT_TIMEOUT" -L -sS \
                -H "Authorization: Bearer $TOKEN" \
                -H "Accept: application/json" \
                -o "$body_file" -D "$meta_file" -w '%{http_code}' "$url" 2>"$stderr_file") || rc=$?
        fi
        printf 'rc=%s\nstatus=%s\n' "$rc" "$status" >"$meta_file.status"
    else
        local rc=0
        if [[ -n "$CACERT_PATH" ]]; then
            "$GOCURL_PATH" --cacert "$CACERT_PATH" --connect-timeout "$HOST_CONNECT_TIMEOUT" \
                --json-output -X GET \
                -H "Authorization: Bearer $TOKEN" \
                -H "Accept: application/json" \
                --url "$url" >"$meta_file.json" 2>"$stderr_file" || rc=$?
        else
            "$GOCURL_PATH" --connect-timeout "$HOST_CONNECT_TIMEOUT" \
                --json-output -X GET \
                -H "Authorization: Bearer $TOKEN" \
                -H "Accept: application/json" \
                --url "$url" >"$meta_file.json" 2>"$stderr_file" || rc=$?
        fi
        python3 - "$meta_file.json" "$body_file" "$meta_file" "$meta_file.status" "$rc" <<'PY'
import base64, json, sys
src, body_path, meta_path, status_path, rc = sys.argv[1:6]
status = '0'
err = ''
body = ''
try:
    raw = open(src, 'r', encoding='utf-8').read().strip()
    if raw:
        data = json.loads(raw)
        status = str(data.get('status_code', 0))
        err = data.get('error', '') or ''
        body64 = data.get('body_base64', '') or ''
        if body64:
            body = base64.b64decode(body64).decode('utf-8', 'replace')
except Exception as exc:
    err = f'gocurl json parse failed: {exc}'
open(body_path, 'w', encoding='utf-8').write(body)
open(meta_path, 'w', encoding='utf-8').write(err)
open(status_path, 'w', encoding='utf-8').write(f'rc={rc}\nstatus={status}\n')
PY
    fi
}

get_status_value() {
    local key=$1 file=$2
    awk -F= -v k="$key" '$1==k {print substr($0, index($0, "=")+1)}' "$file" | tail -n 1
}

json_query() {
    local mode=$1 file=$2
    python3 - "$mode" "$file" <<'PY'
import json, sys
mode, path = sys.argv[1:3]
with open(path, 'r', encoding='utf-8') as fh:
    data = json.load(fh)
if mode == 'libraries':
    libs = data.get('libraries') or []
    print(len(libs))
    ids = [str(x.get('id', '')) for x in libs if isinstance(x, dict)]
    print(' '.join(ids[:5]))
elif mode == 'page':
    results = data.get('results')
    if results is None:
        results = data.get('libraryItems') or []
    first = results[0] if results else {}
    print(len(results))
    print(str(first.get('id', '')))
    title = first.get('title') or first.get('name') or ''
    if not title and isinstance(first.get('media'), dict):
        title = ((first.get('media') or {}).get('metadata') or {}).get('title') or ''
    print(title)
elif mode == 'detail':
    title = data.get('title') or ''
    media = data.get('media') or {}
    meta = media.get('metadata') if isinstance(media, dict) else {}
    if not title and isinstance(meta, dict):
        title = meta.get('title') or ''
    audio = media.get('audioFiles') if isinstance(media, dict) else []
    print(title)
    print(len(audio) if isinstance(audio, list) else 0)
else:
    raise SystemExit(f'unknown mode: {mode}')
PY
}

run_host_step() {
    local label=$1 url=$2 mode=$3 body=$4 meta=$5 stderr=$6
    local rc status stderr_text parse_out

    host_request "$url" "$body" "$meta" "$stderr"
    rc=$(get_status_value rc "$meta.status")
    status=$(get_status_value status "$meta.status")
    stderr_text=$(redact_text "$(cat "$stderr" 2>/dev/null || true)")

    echo "[$label]"
    print_kv url "$(redact_text "$url")"
    print_kv rc "$rc"
    print_kv http_status "$status"
    print_kv response_bytes "$(wc -c <"$body" | tr -d ' ')"
    if [[ -n "$stderr_text" ]]; then
        print_kv stderr "$stderr_text"
    else
        print_kv stderr "(none)"
    fi

    if [[ "$rc" != "0" || "$status" != "200" ]]; then
        return 1
    fi

    parse_out=$(json_query "$mode" "$body") || return 1
    case "$mode" in
        libraries)
            local count ids
            count=$(printf '%s\n' "$parse_out" | sed -n '1p')
            ids=$(printf '%s\n' "$parse_out" | sed -n '2p')
            print_kv libraries "$count"
            print_kv sample_ids "${ids:-'(none)'}"
            if ! grep -Eq "(^| )${LIBRARY_ID}( |$)" <<<"$ids" && ! python3 - "$body" "$LIBRARY_ID" <<'PY'
import json, sys
body, wanted = sys.argv[1:3]
libs = json.load(open(body, 'r', encoding='utf-8')).get('libraries') or []
raise SystemExit(0 if any(str(x.get('id','')) == wanted for x in libs if isinstance(x, dict)) else 1)
PY
            then
                print_kv library_id_check "configured library_id not present in /api/libraries"
                return 1
            fi
            print_kv library_id_check "present"
            ;;
        page)
            local count first title
            count=$(printf '%s\n' "$parse_out" | sed -n '1p')
            first=$(printf '%s\n' "$parse_out" | sed -n '2p')
            title=$(printf '%s\n' "$parse_out" | sed -n '3p')
            print_kv results "$count"
            print_kv first_item_id "${first:-'(none)'}"
            print_kv first_item_title "${title:-'(none)'}"
            [[ -n "$first" ]] || return 1
            FIRST_ITEM_ID=$first
            ;;
        detail)
            local title audio_count
            title=$(printf '%s\n' "$parse_out" | sed -n '1p')
            audio_count=$(printf '%s\n' "$parse_out" | sed -n '2p')
            print_kv detail_title "${title:-'(none)'}"
            print_kv audio_files "$audio_count"
            ;;
    esac

    return 0
}

run_host_api_smoke() {
    local libraries_body="$WORKDIR/libraries.json"
    local libraries_meta="$WORKDIR/libraries.meta"
    local libraries_err="$WORKDIR/libraries.stderr"
    local page_body="$WORKDIR/page0.json"
    local page_meta="$WORKDIR/page0.meta"
    local page_err="$WORKDIR/page0.stderr"
    local detail_body="$WORKDIR/detail.json"
    local detail_meta="$WORKDIR/detail.meta"
    local detail_err="$WORKDIR/detail.stderr"
    local page_url detail_url

    echo "== Host API/config smoke =="
    print_kv cfg_path "$CFG_PATH"
    print_kv server_url "$SERVER_URL"
    print_kv token "$REDACTED_TOKEN"
    print_kv library_id "$LIBRARY_ID"
    print_kv download_dir "${DOWNLOAD_DIR:-'(unset)'}"
    print_kv page_size "$PAGE_SIZE"
    print_kv http_client "$HTTP_CLIENT"
    print_kv cacert "${CACERT_PATH:-'(system default trust store)'}"
    echo

    if ! run_host_step "libraries" "$SERVER_URL/api/libraries" libraries \
        "$libraries_body" "$libraries_meta" "$libraries_err"; then
        echo
        echo "Host API/config result: FAIL"
        HOST_API_OK=0
        return 1
    fi

    page_url="$SERVER_URL/api/libraries/$LIBRARY_ID/items?limit=$PAGE_SIZE&page=0&sort=media.metadata.title&desc=0&minified=1"
    echo
    if ! run_host_step "page-0" "$page_url" page \
        "$page_body" "$page_meta" "$page_err"; then
        echo
        echo "Host API/config result: FAIL"
        HOST_API_OK=0
        return 1
    fi

    detail_url="$SERVER_URL/api/items/$FIRST_ITEM_ID"
    echo
    if ! run_host_step "first-item-detail" "$detail_url" detail \
        "$detail_body" "$detail_meta" "$detail_err"; then
        echo
        echo "Host API/config result: FAIL"
        HOST_API_OK=0
        return 1
    fi

    HOST_API_OK=1
    echo
    echo "Host API/config result: PASS"
}

run_device_check() {
    local remote_dir remote_script remote_out remote_err remote_body local_raw page_url status stdout_text stderr_text exit_code file_size
    local quoted_token quoted_url quoted_out quoted_err quoted_body

    if [[ "$RUN_DEVICE_CHECK" -eq 0 ]]; then
        DEVICE_SKIPPED=1
        return 0
    fi
    have_cmd adb || fail "adb not found for --device-check"

    echo
    echo "== Device gocurl smoke =="
    page_url="$SERVER_URL/api/libraries/$LIBRARY_ID/items?limit=$PAGE_SIZE&page=0&sort=media.metadata.title&desc=0&minified=1"
    remote_dir="/data/local/tmp/abs_smoke_$$"
    remote_script="$remote_dir/run.sh"
    remote_out="$remote_dir/stdout.txt"
    remote_err="$remote_dir/stderr.txt"
    remote_body="$remote_dir/body.json"
    local_raw="$WORKDIR/device-raw.txt"

    quoted_token=$(shell_quote_sq "$TOKEN")
    quoted_url=$(shell_quote_sq "$page_url")
    quoted_out=$(shell_quote_sq "$remote_out")
    quoted_err=$(shell_quote_sq "$remote_err")
    quoted_body=$(shell_quote_sq "$remote_body")

    adb_cmd shell su -c "rm -rf '$remote_dir' && mkdir -p '$remote_dir'"

    cat <<EOF | adb_cmd shell su -c "cat > '$remote_script' && chmod 700 '$remote_script'"
#!/system/bin/sh
TOKEN=$quoted_token
URL=$quoted_url
OUT=$quoted_out
ERR=$quoted_err
BODY=$quoted_body
$DEVICE_GOCURL --cacert $DEFAULT_CACERT_REMOTE --connect-timeout $HOST_CONNECT_TIMEOUT -L -sS --fail \
  -H "Authorization: Bearer \$TOKEN" \
  -H "Accept: application/json" \
  -o "\$BODY" -w "%{http_code}" --url "\$URL" >"\$OUT" 2>"\$ERR"
rc=\$?
size=0
if [ -f "\$BODY" ]; then
  size=\$(wc -c <"\$BODY" 2>/dev/null)
fi
printf 'exit_code=%s\n' "\$rc"
printf 'stdout=%s\n' "\$(cat "\$OUT" 2>/dev/null)"
printf 'stderr<<EOF\n'
cat "\$ERR" 2>/dev/null
printf 'EOF\n'
printf 'body_size=%s\n' "\$size"
EOF

    adb_cmd shell su -c "$remote_script" >"$local_raw" || true
    adb_cmd shell su -c "rm -rf '$remote_dir'" >/dev/null || true

    exit_code=$(awk -F= '/^exit_code=/{print $2}' "$local_raw" | tail -n 1)
    stdout_text=$(awk -F= '/^stdout=/{print substr($0, index($0, "=")+1)}' "$local_raw" | tail -n 1)
    stderr_text=$(python3 - "$local_raw" <<'PY'
import sys
text = open(sys.argv[1], 'r', encoding='utf-8').read()
start = text.find('stderr<<EOF\n')
if start < 0:
    print('')
    raise SystemExit(0)
start += len('stderr<<EOF\n')
end = text.find('\nEOF\n', start)
if end < 0:
    end = len(text)
print(text[start:end], end='')
PY
)
    file_size=$(awk -F= '/^body_size=/{print $2}' "$local_raw" | tail -n 1)
    status=$(printf '%s' "$stdout_text" | tr -d '[:space:]')

    print_kv endpoint "$(redact_text "$page_url")"
    print_kv exit_code "${exit_code:-'(missing)'}"
    print_kv status_stdout "${status:-'(empty)'}"
    print_kv output_file_size "${file_size:-0}"
    if [[ -n "$stderr_text" ]]; then
        print_kv stderr "$(redact_text "$stderr_text")"
    else
        print_kv stderr "(none)"
    fi

    if [[ "${exit_code:-1}" == "0" ]]; then
        DEVICE_OK=1
        echo
        echo "Device helper result: PASS"
    else
        echo
        echo "Device helper result: FAIL"
    fi
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --cfg|--cacert|--http-client|--gocurl|--adb-serial)
                if [[ $# -lt 2 || "$2" == --* ]]; then
                    fail "missing value for $1"
                fi
                case "$1" in
                    --cfg) CFG_PATH=$2 ;;
                    --cacert) CACERT_PATH=$2 ;;
                    --http-client) HTTP_CLIENT=$2 ;;
                    --gocurl) GOCURL_PATH=$2 ;;
                    --adb-serial) ADB_SERIAL=$2 ;;
                esac
                shift 2 ;;
            --pull-cfg)
                PULL_CFG=1; shift ;;
            --pull-cacert)
                PULL_CACERT=1; shift ;;
            --device-check)
                RUN_DEVICE_CHECK=1; shift ;;
            --keep-workdir)
                KEEP_WORKDIR=1; shift ;;
            --help|-h)
                usage; exit 0 ;;
            *)
                fail "unknown argument: $1" ;;
        esac
    done
}

main() {
    parse_args "$@"

    if [[ "$PULL_CFG" -eq 1 || -z "$CFG_PATH" ]]; then
        have_cmd adb || fail "adb not found and no --cfg provided"
        pull_cfg
    fi
    [[ -f "$CFG_PATH" ]] || fail "cfg not found: $CFG_PATH"

    if [[ "$PULL_CACERT" -eq 1 ]]; then
        have_cmd adb || fail "adb not found for --pull-cacert"
        pull_cacert
    fi
    if [[ -n "$CACERT_PATH" ]]; then
        [[ -f "$CACERT_PATH" ]] || fail "cacert not found: $CACERT_PATH"
    fi

    make_workdir
    load_cfg
    resolve_http_client
    if run_host_api_smoke; then
        run_device_check
    else
        DEVICE_SKIPPED=1
    fi

    echo
    echo "== Summary =="
    if [[ "$HOST_API_OK" -eq 1 ]]; then
        echo "API/config: PASS"
    else
        echo "API/config: FAIL"
    fi

    if [[ "$DEVICE_SKIPPED" -eq 1 ]]; then
        echo "Device gocurl: SKIPPED"
    elif [[ "$DEVICE_OK" -eq 1 ]]; then
        echo "Device gocurl: PASS"
    else
        echo "Device gocurl: FAIL"
    fi

    if [[ "$HOST_API_OK" -eq 1 && ( "$DEVICE_SKIPPED" -eq 1 || "$DEVICE_OK" -eq 1 ) ]]; then
        exit 0
    fi
    exit 1
}

main "$@"

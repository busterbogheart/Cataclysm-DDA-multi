#!/usr/bin/env bash
# start-mp.sh  [mode] [args...]
#
# Modes:
#   (no args)            — launch host + client on this machine (local testing)
#   host                 — launch host only           (run on the server machine)
#   client <host-ip>     — launch client only          (run on the laptop)
#
# In all modes, world and character are auto-resolved from save/ with prompts.

GAME_DIR="$(cd "$(dirname "$0")" && pwd)"
if [[ -f "$GAME_DIR/cataclysm-tiles" ]]; then
    GAME="$GAME_DIR/cataclysm-tiles"
elif [[ -f "$GAME_DIR/cataclysm" ]]; then
    GAME="$GAME_DIR/cataclysm"
else
    echo "Error: no game binary found (expected cataclysm-tiles or cataclysm)"
    exit 1
fi
SAVE_DIR="$GAME_DIR/save"
LAST_CFG="$GAME_DIR/.last-mp"
HOST_PORT=8080
CLIENT_DELAY=4  # seconds before launching client (local-both mode only)

# ---------------------------------------------------------------------------
# Parse mode
# ---------------------------------------------------------------------------

MODE="both"
HOST_IP=""

case "${1:-}" in
    host)
        MODE="host"
        shift
        ;;
    client)
        MODE="client"
        HOST_IP="${2:-}"
        shift 2 2>/dev/null || shift 1 2>/dev/null || true
        echo "Pulling latest save from remote..."
        git -C "$GAME_DIR" pull
        echo ""
        if [[ -z "$HOST_IP" ]]; then
            # Discover LAN candidates from ARP cache (hostname + IP)
            arp_ips=()
            arp_labels=()
            while IFS= read -r line; do
                ip=$(echo "$line" | grep -oE '\(([0-9]{1,3}\.){3}[0-9]{1,3}\)' | tr -d '()')
                [[ -z "$ip" || "$ip" =~ \.255$ ]] && continue
                host=$(echo "$line" | awk '{print $1}')
                arp_ips+=("$ip")
                if [[ "$host" == "?" ]]; then
                    arp_labels+=("$ip")
                else
                    arp_labels+=("$host  ($ip)")
                fi
            done < <(arp -a 2>/dev/null)

            # Filter to machines with port 8080 open (parallel, 1s timeout each)
            printf "  Scanning for hosts on port %d..." "$HOST_PORT"
            _tmpfile=$(mktemp)
            for i in "${!arp_ips[@]}"; do
                ( nc -z -w1 "${arp_ips[$i]}" "$HOST_PORT" 2>/dev/null && echo "$i" >> "$_tmpfile" ) &
            done
            wait
            printf " done.\n\n"

            host_ips=()
            host_labels=()
            while IFS= read -r idx; do
                host_ips+=("${arp_ips[$idx]}")
                host_labels+=("${arp_labels[$idx]}")
            done < <(sort -n "$_tmpfile")
            rm -f "$_tmpfile"

            if [[ ${#host_ips[@]} -gt 0 ]]; then
                echo "  Hosts running CDDA:"
                for i in "${!host_labels[@]}"; do
                    printf "  %d) %s\n" "$((i+1))" "${host_labels[$i]}"
                done
                printf "  %d) Enter manually\n" "$((${#host_ips[@]}+1))"
                echo ""
                while true; do
                    read -rp "  Select host: " choice
                    if [[ "$choice" =~ ^[0-9]+$ ]]; then
                        if (( choice >= 1 && choice <= ${#host_ips[@]} )); then
                            HOST_IP="${host_ips[$((choice-1))]}"
                            break
                        elif (( choice == ${#host_ips[@]}+1 )); then
                            read -rp "  Host IP: " HOST_IP
                            break
                        fi
                    fi
                    echo "  Invalid choice."
                done
            else
                echo "  No hosts found on port $HOST_PORT."
                read -rp "  Host IP: " HOST_IP
            fi
            echo ""
        fi
        ;;
esac

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

decode_char_name() {
    local raw="${1##\#}"
    raw="${raw%%.*}"
    python3 -c "import base64; print(base64.b64decode('$raw').decode())" 2>/dev/null
}

get_worlds() {
    [[ -d "$SAVE_DIR" ]] || return
    for d in "$SAVE_DIR"/*/; do
        [[ -d "$d" ]] && basename "$d"
    done
}

get_chars() {
    local world="$1"
    local dir="$SAVE_DIR/$world"
    [[ -d "$dir" ]] || return
    for f in "$dir"/#*.sav.zzip "$dir"/#*.sav; do
        [[ -f "$f" ]] || continue
        decode_char_name "$(basename "$f")"
    done
}

char_exists() {
    local world="$1" name="$2"
    while IFS= read -r c; do
        [[ "$c" == "$name" ]] && return 0
    done < <(get_chars "$world")
    return 1
}

pick_from_list() {
    local prompt="$1"; shift
    local items=("$@")
    local n=${#items[@]}
    echo ""
    echo "$prompt"
    for i in "${!items[@]}"; do
        printf "  %d) %s\n" "$((i+1))" "${items[$i]}"
    done
    echo ""
    while true; do
        read -rp "  Enter number: " choice
        if [[ "$choice" =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= n )); then
            REPLY="${items[$((choice-1))]}"
            return
        fi
        echo "  Invalid choice — enter a number between 1 and $n."
    done
}

# ---------------------------------------------------------------------------
# Resolve world
# ---------------------------------------------------------------------------

WORLD="${1:-}"
HOST_CHAR="${2:-}"
CLIENT_CHAR="${3:-}"

# Last-session shortcut (both-mode only, and only when no args given)
if [[ "$MODE" == "both" && -z "$WORLD" && -f "$LAST_CFG" ]]; then
    last_world=$(sed -n '1p' "$LAST_CFG")
    last_host=$(sed -n '2p' "$LAST_CFG")
    last_client=$(sed -n '3p' "$LAST_CFG")
    echo ""
    echo "Last session:"
    echo "  World  : $last_world"
    echo "  Host   : $last_host"
    echo "  Client : $last_client"
    echo ""
    echo "  1) Same"
    echo "  2) Swap host/client"
    echo "  3) Pick again"
    echo ""
    read -rp "  Enter number: " choice
    case "$choice" in
        1) WORLD="$last_world"; HOST_CHAR="$last_host";   CLIENT_CHAR="$last_client" ;;
        2) WORLD="$last_world"; HOST_CHAR="$last_client"; CLIENT_CHAR="$last_host"   ;;
        *) ;;
    esac
fi

if [[ -z "$WORLD" ]]; then
    IFS=$'\n' read -ra world_arr -d '' < <(get_worlds; printf '\0')
    if [[ ${#world_arr[@]} -eq 0 ]]; then
        echo "Error: no worlds found in save/"; exit 1
    elif [[ ${#world_arr[@]} -eq 1 ]]; then
        WORLD="${world_arr[0]}"
    else
        pick_from_list "Select world:" "${world_arr[@]}"
        WORLD="$REPLY"
    fi
fi

IFS=$'\n' read -ra char_arr -d '' < <(get_chars "$WORLD"; printf '\0')
if [[ ${#char_arr[@]} -eq 0 ]]; then
    echo "Error: no characters found in world '$WORLD'."
    exit 1
fi

# ---------------------------------------------------------------------------
# Resolve character(s) based on mode
# ---------------------------------------------------------------------------

if [[ "$MODE" == "host" || "$MODE" == "both" ]]; then
    if [[ -z "$HOST_CHAR" ]]; then
        pick_from_list "Select HOST character:" "${char_arr[@]}"
        HOST_CHAR="$REPLY"
    fi
fi

# In host mode: write the chosen char to a marker file and push it so the
# client machine can filter it out of its character list after git pull.
if [[ "$MODE" == "host" ]]; then
    HOST_CHAR_FILE="$SAVE_DIR/$WORLD/.host_char"
    printf '%s\n' "$HOST_CHAR" > "$HOST_CHAR_FILE"
    git -C "$GAME_DIR" add -f "$HOST_CHAR_FILE" 2>/dev/null
    if ! git -C "$GAME_DIR" diff --cached --quiet 2>/dev/null; then
        git -C "$GAME_DIR" commit -m "mp: host char" --quiet 2>/dev/null
        git -C "$GAME_DIR" push --quiet 2>/dev/null
    fi
fi

# In client mode: read the host's marker file (synced via git pull above)
# so the host's active character can be excluded from the selection list.
if [[ "$MODE" == "client" ]]; then
    HOST_CHAR_FILE="$SAVE_DIR/$WORLD/.host_char"
    if [[ -f "$HOST_CHAR_FILE" ]]; then
        HOST_CHAR=$(tr -d '[:space:]' < "$HOST_CHAR_FILE")
    fi
fi

if [[ "$MODE" == "client" || "$MODE" == "both" ]]; then
    if [[ -z "$CLIENT_CHAR" ]]; then
        # Exclude the already-chosen/active host char
        remaining=()
        for c in "${char_arr[@]}"; do
            [[ "$c" != "$HOST_CHAR" ]] && remaining+=("$c")
        done
        [[ ${#remaining[@]} -eq 0 ]] && remaining=("${char_arr[@]}")
        pick_from_list "Select CLIENT character:" "${remaining[@]}"
        CLIENT_CHAR="$REPLY"
    fi
fi

# ---------------------------------------------------------------------------
# Validate
# ---------------------------------------------------------------------------

bad=0
if [[ "$MODE" == "host" || "$MODE" == "both" ]]; then
    if ! char_exists "$WORLD" "$HOST_CHAR"; then
        echo "Error: host character \"$HOST_CHAR\" not found in world '$WORLD'."
        bad=1
    fi
fi
if [[ "$MODE" == "client" || "$MODE" == "both" ]]; then
    if ! char_exists "$WORLD" "$CLIENT_CHAR"; then
        echo "Error: client character \"$CLIENT_CHAR\" not found in world '$WORLD'."
        bad=1
    fi
fi

if [[ $bad -ne 0 ]]; then
    echo ""
    echo "Characters in '$WORLD':"
    get_chars "$WORLD" | sed 's/^/  /'
    exit 1
fi

# ---------------------------------------------------------------------------
# Launch
# ---------------------------------------------------------------------------

HOST_LOG=/tmp/cdda-mp-server.log
CLIENT_LOG=/tmp/cdda-mp-client.log

# Kill any stale game instances so port 8080 is free and logs are clean.
pkill -f "cataclysm-tiles --host" 2>/dev/null
pkill -f "cataclysm-tiles --client" 2>/dev/null
pkill -f "cataclysm --host" 2>/dev/null
pkill -f "cataclysm --client" 2>/dev/null
sleep 1

if [[ "$MODE" == "host" ]]; then
    echo ""
    echo "  Mode   : HOST only"
    echo "  World  : $WORLD"
    echo "  Char   : $HOST_CHAR"
    echo "  Port   : $HOST_PORT"
    echo "  Log    : $HOST_LOG"
    echo ""
    echo "  NOTE: If clients can't connect, allow this app through macOS Firewall:"
    echo "    System Settings → Network → Firewall → Options → add cataclysm-tiles"
    echo ""
    > "$HOST_LOG"
    "$GAME" --host --world "$WORLD" --char "$HOST_CHAR" 2>&1 | tee "$HOST_LOG"
    echo ""
    echo "Session ended. Pushing save to remote..."
    # Remove the host-char marker now that the session is over.
    HOST_CHAR_FILE="$SAVE_DIR/$WORLD/.host_char"
    rm -f "$HOST_CHAR_FILE"
    git -C "$GAME_DIR" add -f save/
    if git -C "$GAME_DIR" diff --cached --quiet; then
        echo "No save changes to push."
    else
        git -C "$GAME_DIR" commit -m "save: session $(date '+%Y-%m-%d %H:%M')"
        git -C "$GAME_DIR" push
    fi

elif [[ "$MODE" == "client" ]]; then
    echo ""
    echo "  Mode   : CLIENT only"
    echo "  World  : $WORLD"
    echo "  Char   : $CLIENT_CHAR"
    echo "  Host   : $HOST_IP:$HOST_PORT"
    echo "  Log    : $CLIENT_LOG"
    echo ""
    > "$CLIENT_LOG"
    "$GAME" --client "${HOST_IP}:${HOST_PORT}" --client-name "$CLIENT_CHAR" \
         --world "$WORLD" --char "$CLIENT_CHAR" 2>&1 | tee "$CLIENT_LOG"
    echo ""
    if git -C "$GAME_DIR" status --porcelain save/ | grep -q .; then
        echo "Local save changes detected after client session — discarding (host is authoritative)."
        git -C "$GAME_DIR" checkout -- save/
    fi

else
    # both — local co-op testing
    echo ""
    echo "  World  : $WORLD"
    echo "  Host   : $HOST_CHAR"
    echo "  Client : $CLIENT_CHAR  (joining in ${CLIENT_DELAY}s)"
    echo ""

    printf '%s\n%s\n%s\n' "$WORLD" "$HOST_CHAR" "$CLIENT_CHAR" > "$LAST_CFG"

    > "$HOST_LOG"
    > "$CLIENT_LOG"

    "$GAME" --host --world "$WORLD" --char "$HOST_CHAR" 2>&1 | tee -a "$HOST_LOG" &
    HOST_PID=${PIPESTATUS[0]}

    sleep "$CLIENT_DELAY"

    "$GAME" --client "localhost:${HOST_PORT}" --client-name "$CLIENT_CHAR" \
            --world "$WORLD" --char "$CLIENT_CHAR" 2>&1 | tee -a "$CLIENT_LOG" &
    CLIENT_PID=${PIPESTATUS[0]}

    echo "Host   PID: $HOST_PID"
    echo "Client PID: $CLIENT_PID"
fi

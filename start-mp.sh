#!/usr/bin/env bash
# start-mp.sh  [world] [host-char] [client-char]
#
# Starts a host and client instance for local co-op testing.
# With fewer than 3 arguments it lists available worlds / characters.
# When exactly one option exists for a missing argument it is auto-selected.

GAME_DIR="$(cd "$(dirname "$0")" && pwd)"
GAME="$GAME_DIR/cataclysm-tiles"
SAVE_DIR="$GAME_DIR/save"
LAST_CFG="$GAME_DIR/.last-mp"
HOST_PORT=8080
CLIENT_DELAY=4  # seconds before launching client

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

decode_char_name() {
    # Input: raw basename like "#R2FycmV0dCBLaWxnb3Jl.sav.zzip"
    local raw="${1##\#}"   # strip leading #
    raw="${raw%%.*}"        # strip everything from first dot
    python3 -c "import base64; print(base64.b64decode('$raw').decode())" 2>/dev/null
}

# Print world names, one per line
get_worlds() {
    [[ -d "$SAVE_DIR" ]] || return
    for d in "$SAVE_DIR"/*/; do
        [[ -d "$d" ]] && basename "$d"
    done
}

# Print character names for a world, one per line
get_chars() {
    local world="$1"
    local dir="$SAVE_DIR/$world"
    [[ -d "$dir" ]] || return
    for f in "$dir"/#*.sav.zzip "$dir"/#*.sav; do
        [[ -f "$f" ]] || continue
        decode_char_name "$(basename "$f")"
    done
}

# Return 0 if character exists in world, 1 otherwise
char_exists() {
    local world="$1" name="$2"
    while IFS= read -r c; do
        [[ "$c" == "$name" ]] && return 0
    done < <(get_chars "$world")
    return 1
}

# ---------------------------------------------------------------------------
# Resolve world
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Interactive picker — numbered menu, returns selected value in REPLY
# ---------------------------------------------------------------------------

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
# Resolve world + characters
# ---------------------------------------------------------------------------

WORLD="${1:-}"
HOST_CHAR="${2:-}"
CLIENT_CHAR="${3:-}"

# If no args given and a previous config exists, offer shortcuts
if [[ -z "$WORLD" && -f "$LAST_CFG" ]]; then
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
        *) ;;  # fall through to normal picker
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
if [[ ${#char_arr[@]} -lt 2 ]]; then
    echo "Error: need at least 2 characters in world '$WORLD' (found ${#char_arr[@]})."
    exit 1
fi

if [[ -z "$HOST_CHAR" ]]; then
    pick_from_list "Select HOST character:" "${char_arr[@]}"
    HOST_CHAR="$REPLY"
fi

if [[ -z "$CLIENT_CHAR" ]]; then
    remaining=()
    for c in "${char_arr[@]}"; do
        [[ "$c" != "$HOST_CHAR" ]] && remaining+=("$c")
    done
    pick_from_list "Select CLIENT character:" "${remaining[@]}"
    CLIENT_CHAR="$REPLY"
fi

# ---------------------------------------------------------------------------
# Validate — both characters must exist (prevents accidental new-char creation)
# ---------------------------------------------------------------------------

bad=0
for role_char in "host:$HOST_CHAR" "client:$CLIENT_CHAR"; do
    role="${role_char%%:*}"
    name="${role_char#*:}"
    if ! char_exists "$WORLD" "$name"; then
        echo "Error: $role character \"$name\" not found in world '$WORLD'."
        bad=1
    fi
done

if [[ $bad -ne 0 ]]; then
    echo ""
    echo "Characters in '$WORLD':"
    get_chars "$WORLD" | sed 's/^/  /'
    echo ""
    echo "Tip: wrap names with spaces in quotes, e.g.:"
    echo "  $0 \"$WORLD\" \"Garrett Kilgore\" \"Carrol Alves\""
    exit 1
fi

# ---------------------------------------------------------------------------
# Launch
# ---------------------------------------------------------------------------

echo ""
echo "  World  : $WORLD"
echo "  Host   : $HOST_CHAR"
echo "  Client : $CLIENT_CHAR  (joining in ${CLIENT_DELAY}s)"
echo ""

printf '%s\n%s\n%s\n' "$WORLD" "$HOST_CHAR" "$CLIENT_CHAR" > "$LAST_CFG"

HOST_LOG=/tmp/cdda-mp-server.log
CLIENT_LOG=/tmp/cdda-mp-client.log
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

// The room list for Azangara multiplayer: hosts announce their rooms, lobbies read the list.
//
// GET    /rooms?version=N   open rooms of that protocol version, one per line:
//                           address \t players \t palace \t level \t name
// POST   /rooms             announce or keep alive: version, port, players, palace, level, name, ip
// DELETE /rooms             take the caller's room off: port, ip

const TTL = 90;          // seconds a room stays listed without being announced again
const MAX_PER_IP = 4;    // rooms one address may list at once, one per port of the mod's range
const MAX_LISTED = 50;

export default {
    async fetch(request, env) {
        const url = new URL(request.url);

        if (url.pathname !== "/rooms") {
            return text("not found", 404);
        }

        const now = Math.floor(Date.now() / 1000);

        if (request.method === "GET") {
            await env.DB.prepare("DELETE FROM rooms WHERE seen < ?").bind(now - TTL).run();

            const version = number(url.searchParams.get("version"));
            const { results } = await env.DB
                .prepare("SELECT ip, port, players, palace, level, name FROM rooms WHERE version = ? ORDER BY seen DESC LIMIT ?")
                .bind(version, MAX_LISTED)
                .all();

            const lines = results.map(room =>
                [`${room.ip}:${room.port}`, room.players, room.palace, room.level, room.name].map(field => clean(field)).join("\t"));

            return text(lines.join("\n"));
        }

        const form = new URLSearchParams(await request.text());
        const ip = callerIp(request, form.get("ip"));
        const port = number(form.get("port"));

        if (ip === "" || port < 1 || port > 65535) {
            return text("no IPv4 address or port", 400);
        }

        if (request.method === "DELETE") {
            await env.DB.prepare("DELETE FROM rooms WHERE ip = ? AND port = ?").bind(ip, port).run();
            return text("ok");
        }

        if (request.method !== "POST") {
            return text("method not allowed", 405);
        }

        const others = await env.DB
            .prepare("SELECT COUNT(*) AS count FROM rooms WHERE ip = ? AND port <> ? AND seen >= ?")
            .bind(ip, port, now - TTL)
            .first();

        if (others !== null && others.count >= MAX_PER_IP) {
            return text("too many rooms from this address", 429);
        }

        await env.DB
            .prepare(`INSERT INTO rooms (ip, port, version, players, palace, level, name, seen)
                      VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                      ON CONFLICT (ip, port) DO UPDATE SET version = excluded.version, players = excluded.players,
                          palace = excluded.palace, level = excluded.level, name = excluded.name, seen = excluded.seen`)
            .bind(ip, port, number(form.get("version")), number(form.get("players")),
                  clean(form.get("palace"), 20), clean(form.get("level"), 128), clean(form.get("name"), 24), now)
            .run();

        return text("ok");
    },
};

function callerIp(request, claimed) {
    const seen = request.headers.get("CF-Connecting-IP") || "";

    if (isIpv4(seen)) {
        return seen;
    }

    return isIpv4(claimed) ? claimed : "";
}

function isIpv4(value) {
    const parts = String(value || "").split(".");
    return parts.length === 4 && parts.every(part => /^\d{1,3}$/.test(part) && Number(part) <= 255);
}

function number(value) {
    const parsed = parseInt(value || "0", 10);
    return Number.isFinite(parsed) ? parsed : 0;
}

// Tabs and line breaks would split the list's lines and fields.
function clean(value, length = 128) {
    return String(value ?? "").replace(/[\t\r\n]/g, " ").slice(0, length);
}

function text(body, status = 200) {
    return new Response(body, { status, headers: { "Content-Type": "text/plain; charset=utf-8" } });
}

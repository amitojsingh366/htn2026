import { DEFAULT_GAME_ID } from "./types";
import { Sentry, sentryOptions, actionId, setAction, setGame, reportFailure } from "./telemetry";

export { GameRoom } from "./game-room";

const CORS_HEADERS: Record<string, string> = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type, Authorization, Idempotency-Key, sentry-trace, baggage, x-action-id",
  "Access-Control-Expose-Headers": "x-action-id",
  "Access-Control-Max-Age": "86400",
};

function json(body: unknown, status = 200): Response {
  return Response.json(body, { status, headers: CORS_HEADERS });
}

/**
 * Splits `/api/v1/games/{game_id}/rest` into its game id and remaining route.
 * Unprefixed paths address the default game, so the original endpoints still work.
 */
function resolveRoute(pathname: string): { gameId: string; route: string } {
  const prefix = "/api/v1/games/";

  if (pathname.startsWith(prefix)) {
    const [gameId, ...rest] = pathname.slice(prefix.length).split("/");
    if (gameId) {
      return { gameId, route: `/${rest.join("/")}`.replace(/\/$/, "") || "/" };
    }
  }

  return { gameId: DEFAULT_GAME_ID, route: pathname.replace(/(.)\/$/, "$1") };
}

const handler = {
  async fetch(request: Request, env: Env): Promise<Response> {
    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: CORS_HEADERS });
    }

    const url = new URL(request.url);
    const { gameId, route } = resolveRoute(url.pathname);
    setGame(gameId);
    const correlation = actionId(request.headers.get("x-action-id")) ?? crypto.randomUUID();
    setAction(correlation);
    if (route.startsWith("/gateway/") || route === "/registrations") {
      return env.GAME_ROOM.getByName(gameId).fetch(request);
    }
    if (request.method === "GET" && (url.pathname === "/" || url.pathname.startsWith("/assets/") || /\.(svg|ico|png)$/.test(url.pathname))) {
      return env.ASSETS.fetch(request);
    }
    const stub = env.GAME_ROOM.getByName(gameId);

    // WebSocket upgrades are passed through to the Durable Object untouched.
    const isWebSocketRoute =
      route === "/ws/population" ||
      route === "/ws/device" ||
      route === "/ws/esp" ||
      route.startsWith("/ws/device/") ||
      route.startsWith("/ws/esp/");

    if (isWebSocketRoute && request.headers.get("Upgrade") === "websocket") {
      return stub.fetch(request);
    }

    if (request.method === "GET") {
      switch (route) {
        case "/":
          return json({
            message: "Zombie Tag backend on Workers + Durable Objects",
            game_id: gameId,
            health: "/health",
          });

        case "/health":
          return json({ status: "healthy" });

        case "/population-state":
          return json(await stub.getState(correlation));

        case "/num-players":
          return json({ num_players: (await stub.getState(correlation)).num_players });

        case "/num-infected":
          return json({ num_infected: (await stub.getState(correlation)).num_infected });

        case "/rankings":
        case "/leaderboard":
          return json(await stub.getRankings(gameId));

        case "/device-state": {
          const deviceId = url.searchParams.get("device_id") || url.searchParams.get("deviceId") || "";
          return json(await stub.getDeviceState(deviceId));
        }

        case "/ws/population":
          return json({
            message: `This is a WebSocket endpoint. Connect with ws(s)://${url.host}${url.pathname}`,
            current_state: await stub.getState(correlation),
          });

        case "/ws/device":
        case "/ws/esp":
          return json({
            message: `This is an ESP WebSocket endpoint. Connect with ws(s)://${url.host}${url.pathname}?device_id=<your_device_id>`,
          });
      }
    }

    if (request.method === "POST") {
      switch (route) {
        case "/start-game":
        case "/game/start": {
          try { return json(await stub.startGame(gameId, correlation)); }
          catch (error) { reportFailure(error); return json({ error: error instanceof Error ? error.message : "Unable to prepare game" }, 409); }
        }

        case "/device-event":
        case "/esp/event":
        case "/device-state": {
          let body: unknown;
          try {
            body = await request.json();
          } catch {
            return json({ error: "Invalid JSON body" }, 400);
          }

          try {
            const state = await stub.recordDeviceEvent(body as any, correlation);
            return json({ message: "Device event recorded", ...state });
          } catch (err: any) {
            reportFailure(err);
            return json({ error: err?.message || "Failed to record event" }, 400);
          }
        }

        case "/add-player": {
          try { return json({ message: "Player added", ...await stub.addPlayer(correlation) }); }
          catch (error) { reportFailure(error); return json({ error: error instanceof Error ? error.message : "Cannot add player" }, 409); }
        }

        case "/add-infected": {
          try { return json({ message: "Infected added", ...await stub.addInfected(correlation) }); }
          catch (error) { reportFailure(error); return json({ error: error instanceof Error ? error.message : "Cannot change roles" }, 409); }
        }

        case "/reset-game":
        case "/reset-population": {
          try { return json({ message: "Server game state and registrations cleared", ...await stub.reset(correlation) }); }
          catch (error) { reportFailure(error); return json({ error: error instanceof Error ? error.message : "Cannot reset active round" }, 409); }
        }
      }
    }

    return json({ error: "Not found", route, game_id: gameId }, 404);
  },
} satisfies ExportedHandler<Env>;

export default Sentry.withSentry(sentryOptions, {
  async fetch(request, env, ctx) {
    const correlation = actionId(request.headers.get("x-action-id")) ?? crypto.randomUUID();
    const headers = new Headers(request.headers); headers.set("x-action-id", correlation);
    const response = await handler.fetch(new Request(request, { headers }), env);
    if (response.status === 101) return response;
    const outgoing = new Headers(response.headers);
    for (const [name, value] of Object.entries(CORS_HEADERS)) outgoing.set(name, value);
    outgoing.set("x-action-id", correlation);
    return new Response(response.body, { status: response.status, statusText: response.statusText, headers: outgoing });
  },
} satisfies ExportedHandler<Env>);

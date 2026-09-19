import { DEFAULT_GAME_ID } from "./types";

export { GameRoom } from "./game-room";

const CORS_HEADERS: Record<string, string> = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type, Authorization",
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

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: CORS_HEADERS });
    }

    const url = new URL(request.url);
    const { gameId, route } = resolveRoute(url.pathname);
    const stub = env.GAME_ROOM.getByName(gameId);

    // WebSocket upgrades are passed through to the Durable Object untouched.
    if (route === "/ws/population" && request.headers.get("Upgrade") === "websocket") {
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
          return json(await stub.getState());

        case "/num-players":
          return json({ num_players: (await stub.getState()).num_players });

        case "/num-infected":
          return json({ num_infected: (await stub.getState()).num_infected });

        case "/ws/population":
          return json({
            message: `This is a WebSocket endpoint. Connect with ws(s)://${url.host}${url.pathname}`,
            current_state: await stub.getState(),
          });
      }
    }

    if (request.method === "POST") {
      switch (route) {
        case "/add-player": {
          const state = await stub.addPlayer();
          return json({ message: "Player added", ...state });
        }

        case "/add-infected": {
          const state = await stub.addInfected();
          return json({ message: "Infected added", ...state });
        }

        case "/reset-population": {
          const state = await stub.reset();
          return json({ message: "Population reset", ...state });
        }
      }
    }

    return json({ error: "Not found", route, game_id: gameId }, 404);
  },
} satisfies ExportedHandler<Env>;

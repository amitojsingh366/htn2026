from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
import uvicorn
import json

#TODO - 

app = FastAPI(
    title="Backend",
    description="Backend for Hack The North 2026 hackathon",
    version="0.1.0"
)

# CORS middleware configuration (crucial for frontend/fullstack setups)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# In-memory mock data
num_players = 0
num_infected = 0

def get_current_population_state():
    humans = max(0, num_players - num_infected)
    survived_ratio = (humans / num_players) if num_players > 0 else 0.0
    return {
        "num_players": num_players,
        "num_infected": num_infected,
        "num_humans": humans,
        "survived_pct": round(survived_ratio * 100, 1),
    }

# WebSocket connection manager
class ConnectionManager:
    def __init__(self):
        self.active_connections: list[WebSocket] = []

    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        self.active_connections.append(websocket)
        # Send initial state immediately upon connecting
        await websocket.send_json(get_current_population_state())

    def disconnect(self, websocket: WebSocket):
        if websocket in self.active_connections:
            self.active_connections.remove(websocket)

    async def broadcast(self):
        state = get_current_population_state()
        for connection in list(self.active_connections):
            try:
                await connection.send_json(state)
            except Exception:
                self.disconnect(connection)

manager = ConnectionManager()

# WebSocket Route for true real-time updates
@app.websocket("/ws/population")
async def websocket_population_endpoint(websocket: WebSocket):
    await manager.connect(websocket)
    try:
        while True:
            # Keep the socket open and receive any client ping messages
            await websocket.receive_text()
    except WebSocketDisconnect:
        manager.disconnect(websocket)
    except Exception:
        manager.disconnect(websocket)

# HTTP fallback for browser visits to /ws/population
@app.get("/ws/population", tags=["Population"])
async def get_ws_population_info():
    return {
        "message": "This is a WebSocket endpoint. Connect using ws://localhost:8000/ws/population in React.",
        "current_state": get_current_population_state()
    }

# HTTP Routes
@app.get("/", tags=["General"])
async def root():
    return {
        "message": "Welcome to FastAPI!",
        "docs": "/docs",
        "health": "/health",
    }

@app.get("/population-state", tags=["Population"])
async def get_population_state():
    return get_current_population_state()

@app.post("/add-player")
async def add_player():
    global num_players
    num_players += 1
    await manager.broadcast()
    return {"message": "Player added", "num_players": num_players}

@app.get("/num-players")
async def get_num_players():
    return {"num_players": num_players}

@app.post("/add-infected")
async def add_infected():
    global num_infected
    if num_infected < num_players:
        num_infected += 1
    await manager.broadcast()
    return {"message": "Infected added", "num_infected": num_infected}

@app.get("/num-infected")
async def get_num_infected():
    return {"num_infected": num_infected}

@app.post("/reset-population")
async def reset_population():
    global num_players, num_infected
    num_players = 0
    num_infected = 0
    await manager.broadcast()
    return {"message": "Population reset", "num_players": num_players, "num_infected": num_infected}

@app.get("/health")
async def health():
    return {"status": "healthy"}


if __name__ == "__main__":
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=True)

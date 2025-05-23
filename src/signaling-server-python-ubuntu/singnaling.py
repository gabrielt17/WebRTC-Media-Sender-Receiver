import sys
import ssl
import json
import asyncio
import logging
import websockets

logger = logging.getLogger("websockets")
logger.setLevel(logging.INFO)
logger.addHandler(logging.StreamHandler(sys.stdout))

clients = {}

async def handle_websocket(websocket, path):  # <- Recebe o caminho aqui
    client_id = None
    try:
        splitted = path.strip("/").split("/")
        if not splitted:
            raise Exception("Missing client ID in path")
        client_id = splitted[0]
        print(f"Client {client_id} connected")

        clients[client_id] = websocket
        async for data in websocket:
            print(f"Client {client_id} << {data}")
            message = json.loads(data)
            destination_id = message["id"]
            destination_ws = clients.get(destination_id)
            if destination_ws:
                message["id"] = client_id
                data = json.dumps(message)
                print(f"Client {destination_id} >> {data}")
                await destination_ws.send(data)
            else:
                print(f"Client {destination_id} not found")

    except Exception as e:
        print("Connection error:", e)

    finally:
        if client_id and client_id in clients:
            del clients[client_id]
            print(f"Client {client_id} disconnected")

async def main():
    endpoint = "127.0.0.1"
    port = 8000
    print(f"Listening on {endpoint}:{port}")
    server = await websockets.serve(handle_websocket, endpoint, port)
    await server.wait_closed()

if __name__ == "__main__":
    asyncio.run(main())

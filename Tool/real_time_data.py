import json
import sys
from datetime import datetime
from web_socket import WebSocketOrderBook, MARKET_CHANNEL
from get_id import Get_id


class MarketRealTime:
    WS_URL = "wss://ws-subscriptions-clob.polymarket.com"

    def __init__(self, slug: str):
        self.slug = slug

        # Fetch market details and asset IDs
        slug_id = Get_id(slug)
        slug_data = slug_id.get_market_details()
        self.yes, self.no = json.loads(slug_data['asset_ids'])

        # Set up output file
        self.output_file = open("market_data.txt", "a", encoding="utf-8")
        self.original_stdout = sys.stdout

    def _handle_market_message(self, message: str):
        """Parses and writes price change events to the output file."""
        try:
            data = json.loads(message)
        
            if data.get("event_type") == "book": 
                #I wanted to filter by event type, but it's not working, and it's better to have in the text file all the event_types, so maybe we could remove this part
                print(json.dumps(data))
                sys.stdout.flush()
        except json.JSONDecodeError:
            if message != "PONG":
                print(f"Received non-JSON message: {message}")
                sys.stdout.flush()

    def run(self):
        """Redirect stdout, connect to WebSocket, and stream market data."""
        sys.stdout = self.output_file

        try:
            target_assets = [str(self.yes), str(self.no)]
            auth = {"apiKey": "", "secret": "", "passphrase": ""}

            print(f"\n=== Session started at {datetime.now()} | Slug: {self.slug} ===")
            sys.stdout.flush()

            market_connection = WebSocketOrderBook(
                channel_type=MARKET_CHANNEL,
                url=self.WS_URL,
                data=target_assets,
                auth=auth,
                message_callback=self._handle_market_message,
                verbose=True,
            )

            market_connection.run()

        except KeyboardInterrupt:
            print(f"\n=== Session ended at {datetime.now()} ===")

        finally:
            sys.stdout = self.original_stdout
            self.output_file.close()
    
    print('Have a look at the txt file for updates')
    print('Once you have finished running, press Ctrl+C in the terminal to stop. Then, go to the txt_to_csv.py file to convert everything into a CSV and filter according to your preferences.')
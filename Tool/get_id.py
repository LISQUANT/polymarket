import requests

class Get_id:

    def __init__(self, slug :str):
        self.slug = slug
    
    def get_market_details(self):
        url = f"https://gamma-api.polymarket.com/markets?slug={self.slug}"
        
        try:
            response = requests.get(url)
            response.raise_for_status()
            data = response.json()

            if not data:
                return {"error": "Market not found."}

            # Polymarket usually returns a list; we take the first match
            market = data[0]
            
            return {
                "title": market.get("question"),
                "condition_id": market.get("conditionId"),
                "market_id": market.get("id"),
                "asset_ids": market.get("clobTokenIds"), # Added from your second function
                "active": market.get("active"),
            }

        except requests.exceptions.RequestException as e:
            return {"error": f"API Connection failed: {e}"}

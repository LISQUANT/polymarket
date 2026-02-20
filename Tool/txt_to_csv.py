import json
import pandas as pd
from get_id import Get_id

input_slug = 'btc-updown-15m-1771591500'
input_file = 'market_data.txt'
output_file = 'market_data.csv'
extracted_data = []

with open(input_file, 'r') as f:
    for line in f:
        line = line.strip()
        
        # Skip empty lines or noise
        if not line or line.startswith('=') or line == 'PONG':
            continue
            
        try:
            # Parse the JSON line
            data = json.loads(line)
            
            if isinstance(data, dict):
                if data.get('event_type') == 'price_change':
                    timestamp = data.get('timestamp')
                    # price_changes is a list of asset updates
                    for change in data.get('price_changes', []):
                        extracted_data.append({
                            'timestamp': timestamp,
                            'asset_id': change.get('asset_id'),
                            'price': change.get('price'),
                            'size': change.get('size'),
                            'side': change.get('side'),
                            'best_bid': change.get('best_bid'),
                            'best_ask': change.get('best_ask')
                        })
                        
        except json.JSONDecodeError:
            # This skips lines that aren't valid JSON
            continue

# Create DataFrame and save
df = pd.DataFrame(extracted_data)

slug_id = Get_id(input_slug)
slug_data = slug_id.get_market_details()
yes, no = json.loads(slug_data['asset_ids'])
json.loads(slug_data['asset_ids'])

mapping = {
    f'{yes}': 'YES',
    f'{no}': 'NO'
}
df['asset_id'] = df['asset_id'].replace(mapping)

print(df)

df.to_csv(output_file, index=False)
print(f"Success! Saved {len(df)} rows to {output_file}")
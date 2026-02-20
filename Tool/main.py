from real_time_data import MarketRealTime

input_slug = 'btc-updown-15m-1771612200'

if __name__ == "__main__":
    session = MarketRealTime(input_slug)
    session.run()




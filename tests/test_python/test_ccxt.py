import ccxt
import pandas as pd

exchange = ccxt.okx({
    'proxies': {
        'http': 'http://172.19.160.1:6984',
        'https': 'http://172.19.160.1:6984',
    }
})

all_ohlcv = []
since = None

while len(all_ohlcv) < 666:
    # fetch_ohlcv 是 ccxt 获取 K 线数据的方法，ohlcv 就是 K 线的五个字段首字母：
    # O - Open   开盘价
    # H - High   最高价
    # L - Low    最低价
    # C - Close  收盘价
    # V - Volume 成交量
    ohlcv = exchange.fetch_ohlcv('SOL/USDT', timeframe='1d', since=since, limit=100)
    if not ohlcv:
        break
    all_ohlcv = ohlcv + all_ohlcv
    since = ohlcv[0][0] - 100 * 24 * 60 * 60 * 1000  # 往前推100天(1000是毫秒转换, 100是分页最大数据量)

all_ohlcv = all_ohlcv[-666:]  # 取最新666条

df = pd.DataFrame(all_ohlcv, columns=['timestamp', 'open', 'high', 'low', 'close', 'volume'])
df['date'] = pd.to_datetime(df['timestamp'], unit='ms')
df = df[['date', 'open', 'high', 'low', 'close', 'volume']]

pd.set_option('display.float_format', lambda x: '%.2f' % x)
pd.set_option('display.max_rows', 666)

df.to_csv('/home/dayda/Pegasus_High-Frequency_Trading_System/tests/test_data/SOL_USDT_daily.csv', index=False)
print(df)
print(f"\n共 {len(df)} 条数据")
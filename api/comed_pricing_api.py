"""
ComEd 5-Minute Price API for ESP32 Energy Optimization System
Fetches real-time electricity prices and serves them via REST API
"""

import requests
import json
from flask import Flask, jsonify, request
from flask_cors import CORS
from datetime import datetime, timedelta
import logging
import time
from threading import Thread, Lock
import sqlite3

app = Flask(__name__)
CORS(app)

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

# Global variables for price caching
current_price_data = {
    'price_cents_per_kwh': 0.0,
    'timestamp': None,
    'status': 'initializing',
    'tier': 'unknown',
    'recommendation': 'normal'
}
price_lock = Lock()

# ComEd 5-Minute Price API endpoint
COMED_API_URL = "https://hourlypricing.comed.com/api"
PRICE_ENDPOINT = f"{COMED_API_URL}?type=5minutefeed"
CURRENT_PRICE_ENDPOINT = f"{COMED_API_URL}?type=currenthouraverage"

# Price tier thresholds (cents per kWh)
PRICE_TIERS = {
    'very_low': 3.0,    # Below 3¢ - best time to use energy
    'low': 5.0,         # 3-5¢ - good time
    'normal': 8.0,      # 5-8¢ - normal usage
    'high': 12.0,       # 8-12¢ - reduce usage
    'very_high': 15.0   # Above 15¢ - critical, minimize usage
}

class PriceDatabase:
    """SQLite database for price history"""
    
    def __init__(self, db_path='comed_prices.db'):
        self.db_path = db_path
        self.init_db()
    
    def init_db(self):
        """Initialize database schema"""
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS prices (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT NOT NULL,
                price_cents_per_kwh REAL NOT NULL,
                tier TEXT,
                millisUTC INTEGER
            )
        ''')
        conn.commit()
        conn.close()
    
    def insert_price(self, timestamp, price, tier, millis_utc):
        """Insert price record"""
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        cursor.execute('''
            INSERT INTO prices (timestamp, price_cents_per_kwh, tier, millisUTC)
            VALUES (?, ?, ?, ?)
        ''', (timestamp, price, tier, millis_utc))
        conn.commit()
        conn.close()
    
    def get_recent_prices(self, hours=24):
        """Get price history for last N hours"""
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        cursor.execute('''
            SELECT timestamp, price_cents_per_kwh, tier, millisUTC
            FROM prices
            WHERE timestamp > datetime('now', '-' || ? || ' hours')
            ORDER BY timestamp DESC
        ''', (hours,))
        results = cursor.fetchall()
        conn.close()
        return results
    
    def get_price_stats(self, hours=24):
        """Get statistical summary of recent prices"""
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        cursor.execute('''
            SELECT 
                AVG(price_cents_per_kwh) as avg_price,
                MIN(price_cents_per_kwh) as min_price,
                MAX(price_cents_per_kwh) as max_price,
                COUNT(*) as sample_count
            FROM prices
            WHERE timestamp > datetime('now', '-' || ? || ' hours')
        ''', (hours,))
        result = cursor.fetchone()
        conn.close()
        return {
            'avg_price': result[0] if result[0] else 0,
            'min_price': result[1] if result[1] else 0,
            'max_price': result[2] if result[2] else 0,
            'sample_count': result[3]
        }

# Initialize database
db = PriceDatabase()

def determine_price_tier(price_cents):
    """Determine price tier based on current price"""
    if price_cents < PRICE_TIERS['very_low']:
        return 'very_low'
    elif price_cents < PRICE_TIERS['low']:
        return 'low'
    elif price_cents < PRICE_TIERS['normal']:
        return 'normal'
    elif price_cents < PRICE_TIERS['high']:
        return 'high'
    elif price_cents < PRICE_TIERS['very_high']:
        return 'very_high'
    else:
        return 'critical'

def get_recommendation(tier, stats):
    """Generate energy usage recommendation based on price tier"""
    recommendations = {
        'very_low': {
            'action': 'maximize',
            'message': 'Excellent time to run high-energy appliances',
            'control_mode': 'aggressive_cooling',
            'suggested_temp_offset': -2  # Can cool more
        },
        'low': {
            'action': 'normal_plus',
            'message': 'Good time for normal to high energy use',
            'control_mode': 'normal',
            'suggested_temp_offset': -1
        },
        'normal': {
            'action': 'normal',
            'message': 'Standard energy pricing',
            'control_mode': 'normal',
            'suggested_temp_offset': 0
        },
        'high': {
            'action': 'reduce',
            'message': 'Reduce non-essential energy use',
            'control_mode': 'eco',
            'suggested_temp_offset': 1
        },
        'very_high': {
            'action': 'minimize',
            'message': 'Minimize energy consumption',
            'control_mode': 'aggressive_eco',
            'suggested_temp_offset': 2
        },
        'critical': {
            'action': 'critical',
            'message': 'CRITICAL: Extreme pricing - disable non-essential loads',
            'control_mode': 'emergency',
            'suggested_temp_offset': 3
        }
    }
    
    rec = recommendations.get(tier, recommendations['normal'])
    
    
    if stats and stats['sample_count'] > 0:
        rec['vs_24h_avg'] = current_price_data['price_cents_per_kwh'] - stats['avg_price']
    
    return rec

def fetch_comed_price():
    """Fetch current price from ComEd API"""
    try:
        # Try current hour average first
        response = requests.get(CURRENT_PRICE_ENDPOINT, timeout=10)
        response.raise_for_status()
        
        data = response.json()
        
        if data and len(data) > 0:
            latest = data[0]
            price_cents = float(latest.get('price', 0))
            millis_utc = int(latest.get('millisUTC', 0))
            
            # Convert millisUTC to readable timestamp
            timestamp = datetime.fromtimestamp(millis_utc / 1000.0).isoformat()
            
            # Determine tier and recommendation
            tier = determine_price_tier(price_cents)
            stats = db.get_price_stats(24)
            recommendation = get_recommendation(tier, stats)
            
            # Update global cache
            with price_lock:
                current_price_data.update({
                    'price_cents_per_kwh': price_cents,
                    'timestamp': timestamp,
                    'millisUTC': millis_utc,
                    'status': 'active',
                    'tier': tier,
                    'recommendation': recommendation
                })
            
            # Store in database
            db.insert_price(timestamp, price_cents, tier, millis_utc)
            
            logger.info(f"Updated price: {price_cents}¢/kWh (tier: {tier})")
            return True
        else:
            logger.warning("No data received from ComEd API")
            return False
            
    except requests.exceptions.RequestException as e:
        logger.error(f"Error fetching ComEd price: {e}")
        with price_lock:
            current_price_data['status'] = 'error'
        return False
    except Exception as e:
        logger.error(f"Unexpected error: {e}")
        return False

def price_update_loop():
    """Background thread to update prices every 5 minutes"""
    logger.info("Starting price update loop...")
    
    # Initial fetch
    fetch_comed_price()
    
    while True:
        try:
            time.sleep(300)  # 5 minutes
            fetch_comed_price()
        except Exception as e:
            logger.error(f"Error in update loop: {e}")
            time.sleep(60)  # Retry after 1 minute on error

# API Routes

@app.route('/api/price/current', methods=['GET'])
def get_current_price():
    """Get current electricity price"""
    with price_lock:
        return jsonify(current_price_data)

@app.route('/api/price/esp32', methods=['GET'])
def get_price_for_esp32():
    """
    Optimized endpoint for ESP32 - minimal JSON payload
    Returns only essential data in a compact format
    """
    with price_lock:
        esp32_data = {
            'p': round(current_price_data['price_cents_per_kwh'], 2),  # price
            't': current_price_data['tier'],                           # tier
            'a': current_price_data['recommendation'].get('action', 'normal'),  # action
            'o': current_price_data['recommendation'].get('suggested_temp_offset', 0),  # temp offset
            'ts': current_price_data['millisUTC'],                     # timestamp
            's': current_price_data['status']                          # status
        }
    return jsonify(esp32_data)

@app.route('/api/price/history', methods=['GET'])
def get_price_history():
    """Get historical price data"""
    hours = request.args.get('hours', default=24, type=int)
    hours = min(hours, 168)  # Cap at 1 week
    
    history = db.get_recent_prices(hours)
    
    result = {
        'count': len(history),
        'hours': hours,
        'data': [
            {
                'timestamp': row[0],
                'price_cents_per_kwh': row[1],
                'tier': row[2],
                'millisUTC': row[3]
            }
            for row in history
        ]
    }
    
    return jsonify(result)

@app.route('/api/price/stats', methods=['GET'])
def get_price_statistics():
    """Get statistical summary of prices"""
    hours = request.args.get('hours', default=24, type=int)
    stats = db.get_price_stats(hours)
    
    with price_lock:
        current = current_price_data['price_cents_per_kwh']
    
    result = {
        'period_hours': hours,
        'current_price': current,
        'avg_price': round(stats['avg_price'], 2),
        'min_price': round(stats['min_price'], 2),
        'max_price': round(stats['max_price'], 2),
        'sample_count': stats['sample_count'],
        'current_vs_avg': round(current - stats['avg_price'], 2),
        'current_vs_avg_pct': round((current - stats['avg_price']) / stats['avg_price'] * 100, 1) if stats['avg_price'] > 0 else 0
    }
    
    return jsonify(result)

@app.route('/api/price/forecast', methods=['GET'])
def get_price_forecast():
    """
    Simple forecast based on historical patterns
    Note: ComEd doesn't provide future prices, this is based on typical patterns
    """
    stats = db.get_price_stats(24)
    history = db.get_recent_prices(24)
    
    # Simple forecast: assume similar pattern to yesterday
    forecast = {
        'disclaimer': 'Forecast based on historical patterns, not official ComEd data',
        'next_hour_estimate': round(stats['avg_price'], 2),
        'confidence': 'low',
        'recommendation': 'Check actual prices before making decisions'
    }
    
    return jsonify(forecast)

@app.route('/api/health', methods=['GET'])
def health_check():
    """Health check endpoint"""
    with price_lock:
        status = current_price_data['status']
        last_update = current_price_data['timestamp']
    
    return jsonify({
        'status': 'healthy' if status == 'active' else 'degraded',
        'service': 'comed-pricing-api',
        'last_update': last_update,
        'api_status': status
    })

@app.route('/', methods=['GET'])
def index():
    """API documentation"""
    return jsonify({
        'service': 'ComEd 5-Minute Pricing API for ESP32',
        'version': '1.0',
        'endpoints': {
            '/api/price/current': 'Get current price with full details',
            '/api/price/esp32': 'Optimized endpoint for ESP32 (compact JSON)',
            '/api/price/history?hours=24': 'Get price history',
            '/api/price/stats?hours=24': 'Get price statistics',
            '/api/price/forecast': 'Get simple price forecast',
            '/api/health': 'Health check'
        },
        'update_interval': '5 minutes',
        'data_source': 'ComEd Hourly Pricing API'
    })

if __name__ == '__main__':
    import os
    port = int(os.environ.get('PORT', 5000))
    
    update_thread = Thread(target=price_update_loop, daemon=True)
    update_thread.start()
    
    logger.info("Starting ComEd Pricing API server...")
    app.run(host='0.0.0.0', port=port, debug=False)

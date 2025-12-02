"""
Test script for ComEd Pricing Integration with ESP32 System
Simulates various pricing scenarios and verifies system responses
"""

import requests
import json
import time
from datetime import datetime

# Configuration
API_BASE_URL = "http://localhost:5000"

class Colors:
    """ANSI color codes for terminal output"""
    HEADER = '\033[95m'
    OKBLUE = '\033[94m'
    OKCYAN = '\033[96m'
    OKGREEN = '\033[92m'
    WARNING = '\033[93m'
    FAIL = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'
    UNDERLINE = '\033[4m'

def print_header(text):
    print(f"\n{Colors.HEADER}{Colors.BOLD}{'='*60}{Colors.ENDC}")
    print(f"{Colors.HEADER}{Colors.BOLD}{text.center(60)}{Colors.ENDC}")
    print(f"{Colors.HEADER}{Colors.BOLD}{'='*60}{Colors.ENDC}\n")

def print_success(text):
    print(f"{Colors.OKGREEN}✓ {text}{Colors.ENDC}")

def print_error(text):
    print(f"{Colors.FAIL}✗ {text}{Colors.ENDC}")

def print_info(text):
    print(f"{Colors.OKCYAN}ℹ {text}{Colors.ENDC}")

def print_warning(text):
    print(f"{Colors.WARNING}⚠ {text}{Colors.ENDC}")

def test_api_connection():
    """Test 1: Verify API server is running"""
    print_header("Test 1: API Connection")
    
    try:
        response = requests.get(f"{API_BASE_URL}/api/health", timeout=5)
        if response.status_code == 200:
            data = response.json()
            print_success(f"API server is running")
            print_info(f"Status: {data.get('status')}")
            print_info(f"Last update: {data.get('last_update')}")
            return True
        else:
            print_error(f"API returned status code: {response.status_code}")
            return False
    except requests.exceptions.RequestException as e:
        print_error(f"Cannot connect to API: {e}")
        print_warning("Make sure the API server is running: python comed_pricing_api.py")
        return False

def test_current_price():
    """Test 2: Fetch current price"""
    print_header("Test 2: Current Price Endpoint")
    
    try:
        response = requests.get(f"{API_BASE_URL}/api/price/current", timeout=5)
        if response.status_code == 200:
            data = response.json()
            print_success("Successfully fetched current price")
            print_info(f"Price: {data['price_cents_per_kwh']:.2f}¢/kWh")
            print_info(f"Tier: {data['tier']}")
            print_info(f"Status: {data['status']}")
            print_info(f"Timestamp: {data['timestamp']}")
            
            if 'recommendation' in data:
                rec = data['recommendation']
                print_info(f"Action: {rec.get('action')}")
                print_info(f"Message: {rec.get('message')}")
            
            return True
        else:
            print_error(f"Failed with status code: {response.status_code}")
            return False
    except Exception as e:
        print_error(f"Error: {e}")
        return False

def test_esp32_endpoint():
    """Test 3: Test ESP32-optimized endpoint"""
    print_header("Test 3: ESP32 Optimized Endpoint")
    
    try:
        response = requests.get(f"{API_BASE_URL}/api/price/esp32", timeout=5)
        if response.status_code == 200:
            data = response.json()
            print_success("Successfully fetched ESP32 data")
            print_info(f"Compact payload size: {len(response.content)} bytes")
            print_info(f"Price (p): {data['p']}¢/kWh")
            print_info(f"Tier (t): {data['t']}")
            print_info(f"Action (a): {data['a']}")
            print_info(f"Temp offset (o): {data['o']}")
            print_info(f"Status (s): {data['s']}")
            
            # Verify payload is compact enough for ESP32
            if len(response.content) < 200:
                print_success("Payload size is optimal for ESP32")
            else:
                print_warning("Payload might be too large for optimal ESP32 performance")
            
            return True
        else:
            print_error(f"Failed with status code: {response.status_code}")
            return False
    except Exception as e:
        print_error(f"Error: {e}")
        return False

def test_price_statistics():
    """Test 4: Fetch price statistics"""
    print_header("Test 4: Price Statistics")
    
    try:
        response = requests.get(f"{API_BASE_URL}/api/price/stats?hours=24", timeout=5)
        if response.status_code == 200:
            data = response.json()
            print_success("Successfully fetched price statistics")
            print_info(f"Current price: {data['current_price']:.2f}¢/kWh")
            print_info(f"24h average: {data['avg_price']:.2f}¢/kWh")
            print_info(f"24h minimum: {data['min_price']:.2f}¢/kWh")
            print_info(f"24h maximum: {data['max_price']:.2f}¢/kWh")
            print_info(f"Samples: {data['sample_count']}")
            print_info(f"vs Average: {data['current_vs_avg']:+.2f}¢ ({data['current_vs_avg_pct']:+.1f}%)")
            
            return True
        else:
            print_error(f"Failed with status code: {response.status_code}")
            return False
    except Exception as e:
        print_error(f"Error: {e}")
        return False

def test_price_history():
    """Test 5: Fetch price history"""
    print_header("Test 5: Price History")
    
    try:
        response = requests.get(f"{API_BASE_URL}/api/price/history?hours=6", timeout=5)
        if response.status_code == 200:
            data = response.json()
            print_success(f"Successfully fetched {data['count']} price records")
            
            if data['count'] > 0:
                print_info(f"Time range: {data['hours']} hours")
                print_info("Recent prices:")
                
                # Show last 5 entries
                for record in data['data'][:5]:
                    ts = record['timestamp']
                    price = record['price_cents_per_kwh']
                    tier = record['tier']
                    print(f"  {ts} | {price:.2f}¢/kWh | {tier}")
            else:
                print_warning("No historical data available yet")
                print_info("Price history will accumulate over time")
            
            return True
        else:
            print_error(f"Failed with status code: {response.status_code}")
            return False
    except Exception as e:
        print_error(f"Error: {e}")
        return False

def simulate_control_logic():
    """Test 6: Simulate ESP32 control logic"""
    print_header("Test 6: Control Logic Simulation")
    
    try:
        # Fetch current price
        response = requests.get(f"{API_BASE_URL}/api/price/esp32", timeout=5)
        if response.status_code != 200:
            print_error("Failed to fetch price data")
            return False
        
        data = response.json()
        tier = data['t']
        action = data['a']
        temp_offset = data['o']
        
        print_info(f"Current tier: {tier}")
        print_info(f"Recommended action: {action}")
        print_info(f"Temperature offset: {temp_offset:+d}°C")
        
        # Simulate different scenarios
        scenarios = [
            {
                'name': 'Occupied room, warm temperature',
                'temp': 26.0,
                'motion': True,
                'power': 300
            },
            {
                'name': 'Empty room',
                'temp': 24.0,
                'motion': False,
                'power': 50
            },
            {
                'name': 'High power consumption',
                'temp': 25.0,
                'motion': True,
                'power': 600
            }
        ]
        
        print("\nSimulating control decisions:")
        print("-" * 60)
        
        for scenario in scenarios:
            print(f"\n{Colors.BOLD}Scenario: {scenario['name']}{Colors.ENDC}")
            print(f"  Temperature: {scenario['temp']}°C")
            print(f"  Motion: {scenario['motion']}")
            print(f"  Power: {scenario['power']}W")
            print(f"  Price tier: {tier}")
            
            # Determine fan state based on logic
            fan_on = False
            reason = ""
            
            if tier in ['very_low', 'low']:
                if scenario['motion'] and scenario['temp'] > 24.0:
                    fan_on = True
                    reason = "Low prices, room occupied and warm"
                else:
                    reason = "Low prices but conditions don't warrant fan"
            
            elif tier == 'normal':
                if scenario['motion'] and scenario['temp'] > 25.0:
                    fan_on = True
                    reason = "Normal prices, room warm and occupied"
                else:
                    reason = "Normal prices, conserving energy"
            
            elif tier == 'high':
                if scenario['motion'] and scenario['temp'] > 26.0:
                    fan_on = True
                    reason = "High prices but room very warm"
                else:
                    reason = "High prices, minimizing loads"
            
            elif tier in ['very_high', 'critical']:
                fan_on = False
                reason = "Critical pricing, all non-essentials OFF"
            
            # Additional check for high power + high prices
            if scenario['power'] > 500 and tier in ['high', 'very_high', 'critical']:
                fan_on = False
                reason += " (High power + high prices)"
            
            status = f"{Colors.OKGREEN}ON{Colors.ENDC}" if fan_on else f"{Colors.WARNING}OFF{Colors.ENDC}"
            print(f"  → Fan: {status}")
            print(f"  → Reason: {reason}")
        
        return True
        
    except Exception as e:
        print_error(f"Error: {e}")
        return False

def calculate_savings():
    """Test 7: Calculate potential savings"""
    print_header("Test 7: Savings Calculation")
    
    try:
        # Fetch statistics
        response = requests.get(f"{API_BASE_URL}/api/price/stats?hours=24", timeout=5)
        if response.status_code != 200:
            print_error("Failed to fetch statistics")
            return False
        
        data = response.json()
        current_price = data['current_price']
        avg_price = data['avg_price']
        max_price = data['max_price']
        min_price = data['min_price']
        
        print_info(f"Current price: {current_price:.2f}¢/kWh")
        print_info(f"24h average: {avg_price:.2f}¢/kWh")
        print_info(f"Price range: {min_price:.2f} - {max_price:.2f}¢/kWh")
        
        # Simulate savings scenarios
        print("\nPotential Savings Scenarios:")
        print("-" * 60)
        
        # Scenario 1: Avoid running fan during peak prices
        fan_power_w = 100
        hours_avoided = 2
        energy_kwh = (fan_power_w * hours_avoided) / 1000
        savings_peak = energy_kwh * (max_price - avg_price) / 100
        
        print(f"\n{Colors.BOLD}Scenario 1: Avoid fan during peak prices{Colors.ENDC}")
        print(f"  Fan power: {fan_power_w}W")
        print(f"  Hours avoided: {hours_avoided}h")
        print(f"  Energy saved: {energy_kwh:.2f} kWh")
        print(f"  Price difference: {max_price - avg_price:.2f}¢/kWh")
        print_success(f"Savings per event: ${savings_peak:.3f}")
        print_info(f"Monthly (10 events): ${savings_peak * 10:.2f}")
        print_info(f"Annual: ${savings_peak * 10 * 12:.2f}")
        
        # Scenario 2: Pre-cool during low prices
        ac_power_w = 1500
        precool_hours = 1
        energy_kwh_ac = (ac_power_w * precool_hours) / 1000
        savings_precool = energy_kwh_ac * (avg_price - min_price) / 100
        
        print(f"\n{Colors.BOLD}Scenario 2: Pre-cool during low prices{Colors.ENDC}")
        print(f"  AC power: {ac_power_w}W")
        print(f"  Pre-cool duration: {precool_hours}h")
        print(f"  Energy: {energy_kwh_ac:.2f} kWh")
        print(f"  Price difference: {avg_price - min_price:.2f}¢/kWh")
        print_success(f"Savings per day: ${savings_precool:.3f}")
        print_info(f"Monthly: ${savings_precool * 30:.2f}")
        print_info(f"Annual: ${savings_precool * 365:.2f}")
        
        # Total system savings
        total_annual = (savings_peak * 10 * 12) + (savings_precool * 365)
        print(f"\n{Colors.OKGREEN}{Colors.BOLD}Total estimated annual savings: ${total_annual:.2f}{Colors.ENDC}")
        
        # Scale to 100 dorm rooms
        dorm_savings = total_annual * 100
        print_info(f"Savings for 100 dorm rooms: ${dorm_savings:,.2f}/year")
        
        return True
        
    except Exception as e:
        print_error(f"Error: {e}")
        return False

def test_api_performance():
    """Test 8: API performance"""
    print_header("Test 8: API Performance")
    
    try:
        # Test response times
        endpoints = [
            '/api/price/current',
            '/api/price/esp32',
            '/api/price/stats',
            '/api/health'
        ]
        
        print("Testing response times (10 requests each):")
        print("-" * 60)
        
        for endpoint in endpoints:
            times = []
            for _ in range(10):
                start = time.time()
                response = requests.get(f"{API_BASE_URL}{endpoint}", timeout=5)
                elapsed = (time.time() - start) * 1000  # Convert to ms
                times.append(elapsed)
            
            avg_time = sum(times) / len(times)
            min_time = min(times)
            max_time = max(times)
            
            status = "✓" if avg_time < 100 else "⚠"
            print(f"{status} {endpoint}")
            print(f"    Avg: {avg_time:.1f}ms | Min: {min_time:.1f}ms | Max: {max_time:.1f}ms")
            
            if avg_time < 50:
                print(f"    {Colors.OKGREEN}Excellent performance{Colors.ENDC}")
            elif avg_time < 100:
                print(f"    {Colors.OKGREEN}Good performance{Colors.ENDC}")
            else:
                print(f"    {Colors.WARNING}Consider optimization{Colors.ENDC}")
        
        return True
        
    except Exception as e:
        print_error(f"Error: {e}")
        return False

def run_all_tests():
    """Run all tests"""
    print(f"\n{Colors.HEADER}{Colors.BOLD}")
    print("╔════════════════════════════════════════════════════════════╗")
    print("║  ESP32 ComEd Pricing Integration - Test Suite            ║")
    print("╚════════════════════════════════════════════════════════════╝")
    print(f"{Colors.ENDC}")
    
    tests = [
        ("API Connection", test_api_connection),
        ("Current Price", test_current_price),
        ("ESP32 Endpoint", test_esp32_endpoint),
        ("Price Statistics", test_price_statistics),
        ("Price History", test_price_history),
        ("Control Logic", simulate_control_logic),
        ("Savings Calculation", calculate_savings),
        ("API Performance", test_api_performance)
    ]
    
    results = []
    
    for test_name, test_func in tests:
        try:
            result = test_func()
            results.append((test_name, result))
            time.sleep(1)  # Brief pause between tests
        except KeyboardInterrupt:
            print("\n\nTests interrupted by user")
            break
        except Exception as e:
            print_error(f"Unexpected error in {test_name}: {e}")
            results.append((test_name, False))
    
    # Summary
    print_header("Test Summary")
    
    passed = sum(1 for _, result in results if result)
    total = len(results)
    
    print(f"Tests passed: {passed}/{total}")
    print()
    
    for test_name, result in results:
        status = f"{Colors.OKGREEN}PASS{Colors.ENDC}" if result else f"{Colors.FAIL}FAIL{Colors.ENDC}"
        print(f"  {status} - {test_name}")
    
    print()
    
    if passed == total:
        print(f"{Colors.OKGREEN}{Colors.BOLD}All tests passed! ✓{Colors.ENDC}")
        print_info("The smart energy system is ready for deployment")
    else:
        print(f"{Colors.WARNING}{Colors.BOLD}Some tests failed{Colors.ENDC}")
        print_warning("Review the errors above and fix issues before deployment")
    
    print()

if __name__ == "__main__":
    run_all_tests()

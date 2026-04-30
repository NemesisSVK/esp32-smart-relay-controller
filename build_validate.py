# PlatformIO/SCons build script - validates config.json before build
# type: ignore  # Pylance/SCons environment variables
import os
import json
import datetime
import shutil
import re

print("Build validation script loaded - config validation active")

# Make functions available for testing when not in SCons environment
IN_SCONS = False
try:
    from SCons.Script import Import  # noqa: F821 - Provided by SCons
    Import("env")  # noqa: F821 - Provided by SCons
    IN_SCONS = True
except ImportError:
    IN_SCONS = False

def validate_code_date(project_dir):
    """Validate src/CodeDate.h CODE_DATE format and Change log entry.

    Enforces:
    - CODE_DATE matches YYYYMMDD-HHMM
    - The first (newest) Change log bullet references the same CODE_DATE and has a description
    """
    code_date_path = os.path.join(project_dir, "src", "CodeDate.h")
    if not os.path.exists(code_date_path):
        print("Warning: src/CodeDate.h not found; skipping CODE_DATE validation")
        return

    with open(code_date_path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()

    code_date_value = None
    for line in lines:
        match = re.match(r'^\s*#define\s+CODE_DATE\s+"(\d{8}-\d{4})"\s*$', line)
        if match:
            code_date_value = match.group(1)
            break

    if not code_date_value:
        raise Exception("src/CodeDate.h must define CODE_DATE as \"YYYYMMDD-HHMM\"")

    change_log_idx = None
    for idx, line in enumerate(lines):
        if "Change log" in line:
            change_log_idx = idx
            break

    if change_log_idx is None:
        raise Exception("src/CodeDate.h must contain a 'Change log' comment block")

    bullets = []
    for line in lines[change_log_idx + 1 :]:
        bullet_match = re.match(r'^\s*//\s*-\s*(\d{8}-\d{4})\s+UTC:\s*(.+?)\s*$', line)
        if bullet_match:
            bullets.append((bullet_match.group(1), bullet_match.group(2)))
            continue

        # Stop once we hit the CODE_DATE define or a non-comment section.
        if line.strip().startswith("#define CODE_DATE"):
            break
        if line.strip().startswith("#define") or line.strip().startswith("#include"):
            break

    if not bullets:
        raise Exception("src/CodeDate.h Change log must include at least one entry like: // - YYYYMMDD-HHMM UTC: description")

    newest_date, newest_desc = bullets[0]
    if newest_date != code_date_value:
        raise Exception(
            f"src/CodeDate.h Change log newest entry must match CODE_DATE ({code_date_value}); found {newest_date}"
        )
    if not newest_desc or newest_desc.strip() == "":
        raise Exception("src/CodeDate.h Change log newest entry must include a short description after 'UTC:'")

def validate_ip_whitelist(security_config):
    """Validate IP whitelist configuration"""
    # Validate ip_whitelist_enabled
    whitelist_enabled = security_config.get("ip_whitelist_enabled", False)
    if not isinstance(whitelist_enabled, bool):
        raise Exception("security.ip_whitelist_enabled must be a boolean")

    # Validate ip_ranges
    ip_ranges = security_config.get("ip_ranges", [])
    if not isinstance(ip_ranges, list):
        raise Exception("security.ip_ranges must be an array")

    if len(ip_ranges) > 50:
        raise Exception("security.ip_ranges cannot exceed 50 entries")

    for i, ip_range in enumerate(ip_ranges):
        if not isinstance(ip_range, str):
            raise Exception(f"security.ip_ranges[{i}] must be a string")

        # Validate IP or range format
        if not is_valid_ip_or_range(ip_range):
            if '-' in ip_range:
                # Provide helpful guidance for range format
                print(f"Error: Invalid IP range format: {ip_range}")
                print("Format: 'baseIP-suffix' where suffix is the number of additional IPs to include")
                print("Example: '192.168.11.200-13' means IPs 192.168.11.200 through 192.168.11.213")
                print("For your range, if you want 192.168.11.200-213, use '192.168.11.200-13'")
            raise Exception(f"security.ip_ranges[{i}] is not a valid IP address or range: {ip_range}")

def is_valid_ip_or_range(range_str):
    """Validate IP address or range format (same logic as C++ code)"""
    import ipaddress

    if '-' in range_str:
        # Range format: base-suffix (suffix = number of additional IPs)
        try:
            base_ip_str, suffix_str = range_str.split('-', 1)
            base_ip = ipaddress.IPv4Address(base_ip_str)
            suffix = int(suffix_str)
            if not (1 <= suffix <= 255):
                return False
            if base_ip.packed[3] + suffix > 255:
                print(f"Debug: Range '{range_str}' is invalid - base octet {base_ip.packed[3]} + suffix {suffix} = {base_ip.packed[3] + suffix} > 255")
                print(f"Debug: For range from {base_ip.packed[3]} to {base_ip.packed[3] + suffix}, use suffix {suffix} but it exceeds octet limit")
                return False
            return True
        except:
            return False
    else:
        # Single IP format
        try:
            ipaddress.IPv4Address(range_str)
            return True
        except:
            return False

def validate_schedules(config):
    """Validate schedule configuration"""
    schedules = config.get("schedules", {})

    # Check enabled flag
    enabled = schedules.get("enabled", True)
    if not isinstance(enabled, bool):
        raise Exception("schedules.enabled must be a boolean")

    # Check entries array
    entries = schedules.get("entries", [])
    if not isinstance(entries, list):
        raise Exception("schedules.entries must be an array")

    if len(entries) > 50:
        raise Exception("schedules.entries cannot exceed 50 entries")

    for i, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise Exception(f"schedules.entries[{i}] must be an object")

        # Validate day (0-6)
        day = entry.get("day", 0)
        if not isinstance(day, int) or day < 0 or day > 6:
            raise Exception(f"schedules.entries[{i}].day must be 0-6 (0=Monday, 6=Sunday)")

        # Validate start_time format (HH:MM)
        start_time = entry.get("start_time", "")
        if not isinstance(start_time, str) or len(start_time) != 5 or start_time[2] != ':':
            raise Exception(f"schedules.entries[{i}].start_time must be in HH:MM format")

        try:
            start_hour = int(start_time[:2])
            start_minute = int(start_time[3:])
            if not (0 <= start_hour <= 23 and 0 <= start_minute <= 59):
                raise ValueError()
        except (ValueError, IndexError):
            raise Exception(f"schedules.entries[{i}].start_time has invalid time values")

        # Validate end_time format (HH:MM)
        end_time = entry.get("end_time", "")
        if not isinstance(end_time, str) or len(end_time) != 5 or end_time[2] != ':':
            raise Exception(f"schedules.entries[{i}].end_time must be in HH:MM format")

        try:
            end_hour = int(end_time[:2])
            end_minute = int(end_time[3:])
            if not (0 <= end_hour <= 23 and 0 <= end_minute <= 59):
                raise ValueError()
        except (ValueError, IndexError):
            raise Exception(f"schedules.entries[{i}].end_time has invalid time values")

        # Validate start_level
        start_level = entry.get("start_level", "")
        if start_level not in ["off", "low", "mid", "high"]:
            raise Exception(f"schedules.entries[{i}].start_level must be 'off', 'low', 'mid', or 'high'")

        # Note: end_level is always "off" by design, not stored in config
        # Note: priority is not implemented in current schedule system

def validate_config(project_dir):
    """Validate config.json before build"""
    print("=" * 50)
    print("Validating config.json...")
    print("=" * 50)

    config_path = os.path.join(project_dir, "config.json")
    if not os.path.exists(config_path):
        print("Error: config.json not found")
        raise Exception("config.json not found - cannot continue build")

    try:
        with open(config_path, 'r') as f:
            config = json.load(f)
        print("config.json is valid JSON")
    except json.JSONDecodeError as e:
        print(f"Error: config.json is invalid JSON: {e}")
        raise Exception(f"Invalid config.json - build cannot continue: {e}")

    # Check for required sections
    required_sections = ["wifi", "mqtt", "sensors", "device_info", "network", "security", "ntp"]
    for section in required_sections:
        if section not in config:
            print(f"Error: Missing required section '{section}' in config.json")
            raise Exception(f"Missing required section '{section}' in config.json")

    # Check wifi
    wifi = config.get("wifi", {})
    if not wifi.get("ssid"):
        print("Warning: WiFi SSID is empty")
    if not wifi.get("password"):
        print("Warning: WiFi password is empty")

    # Check mqtt
    mqtt = config.get("mqtt", {})
    if not mqtt.get("server"):
        print("Warning: MQTT server is empty")
    if not isinstance(mqtt.get("port"), int) or mqtt.get("port") <= 0:
        print("Error: Invalid MQTT port")
        raise Exception("Invalid MQTT port in config.json")

    # Check sensors
    sensors = config.get("sensors", {})
    if not isinstance(sensors.get("one_wire_bus_count"), int):
        print("Error: Invalid one_wire_bus_count")
        raise Exception("Invalid one_wire_bus_count in config.json")

    sensor_addresses = sensors.get("sensor_addresses", {})
    for i in range(1, sensors.get("sensor_count", 1) + 1):
        addr_key = f"sensor{i}_address"
        addr = sensor_addresses.get(addr_key, "")
        if addr:
            # Check if hex string is 16 characters (8 bytes * 2 hex chars per byte)
            if len(addr) != 16:
                print(f"Error: Sensor {i} address must be 16-character hex string")
                raise Exception(f"Invalid sensor {i} address in config.json")
            # Validate hex format
            try:
                int(addr, 16)
            except ValueError:
                print(f"Error: Sensor {i} address must be valid hex string")
                raise Exception(f"Invalid sensor {i} address in config.json")

    # Check MQTT timing configuration
    mqtt_publish_interval = config.get("mqtt", {}).get("publish_interval")
    if mqtt_publish_interval is None:
        print("Error: No MQTT timing configuration found")
        raise Exception("No MQTT timing configuration found in config.json")
    elif not isinstance(mqtt_publish_interval, int) or mqtt_publish_interval <= 0:
        print("Error: Invalid mqtt publish_interval")
        raise Exception("Invalid mqtt publish_interval in config.json")

    # Check device_info
    device_info = config.get("device_info", {})
    if not device_info.get("name"):
        print("Warning: Device name is empty")

    # Check network
    network = config.get("network", {})
    if not network.get("hostname"):
        print("Warning: Network hostname is empty")

    # Check security
    security = config.get("security", {})
    if not isinstance(security.get("auth_enabled"), bool):
        print("Error: Invalid security auth_enabled")
        raise Exception("Invalid security auth_enabled in config.json")
    if security.get("auth_enabled"):
        if not security.get("username"):
            print("Error: Security username required when auth is enabled")
            raise Exception("Security username required when auth is enabled")
        if not security.get("password_hash"):
            print("Error: Security password_hash required when auth is enabled")
            raise Exception("Security password_hash required when auth is enabled")

    # Check IP whitelist
    try:
        validate_ip_whitelist(security)
        print("IP whitelist configuration validated successfully")
    except Exception as e:
        print(f"Error: {e}")
        raise

    # Check ntp
    ntp = config.get("ntp", {})
    servers = ntp.get("servers", [])
    if len(servers) > 3:
        print("Error: Maximum 3 NTP servers allowed")
        raise Exception("Too many NTP servers in config.json")
    for server in servers:
        if not isinstance(server, str) or len(server) == 0:
            print("Error: NTP servers must be non-empty strings")
            raise Exception("Invalid NTP server in config.json")
    timezone = ntp.get("timezone", "")
    if len(timezone) > 200:
        print("Error: NTP timezone string too long")
        raise Exception("NTP timezone string too long in config.json")

    # Check schedules
    try:
        validate_schedules(config)
        print("Schedule configuration validated successfully")
    except Exception as e:
        print(f"Error: {e}")
        raise

    # Validate CODE_DATE discipline and Change log formatting
    try:
        validate_code_date(project_dir)
        print("CODE_DATE validated successfully")
    except Exception as e:
        print(f"Error: {e}")
        raise

    print("config.json validation completed successfully")
    print("=" * 50)

    # Generate BuildConfig.h with build timestamp
    generate_build_config(project_dir)

    # Validate ota.json (required for espota uploads)
    try:
        ota_path = os.path.join(project_dir, "ota.json")
        upload_protocol = ""
        if IN_SCONS:
            try:
                upload_protocol = env.subst("$UPLOAD_PROTOCOL")  # noqa: F821 - Provided by SCons
            except Exception:
                upload_protocol = ""

        # Only validate when present, unless we're doing an OTA (espota) upload.
        if os.path.exists(ota_path) or upload_protocol == "espota":
            validate_ota_json(project_dir)
    except Exception as e:
        # Fail hard only when building for OTA upload
        if upload_protocol == "espota":
            raise
        print(f"Warning: ota.json validation failed: {e}")

def generate_build_config(project_dir):
    """Generate BuildConfig.h with build info"""
    build_config_path = os.path.join(project_dir, "src", "BuildConfig.h")

    build_timestamp = datetime.datetime.utcnow().isoformat() + 'Z'

    content = f"""#ifndef BUILD_CONFIG_H
#define BUILD_CONFIG_H

// Auto-generated during build
// Build timestamp: {build_timestamp}

#define BUILD_TIMESTAMP "{build_timestamp}"

#endif // BUILD_CONFIG_H
"""

    with open(build_config_path, 'w') as f:
        f.write(content)

    print(f"Generated {build_config_path} with build timestamp")

def validate_ota_json(project_dir):
    """Validate ota.json schema (used by host upload scripts and device OTA)."""
    ota_path = os.path.join(project_dir, "ota.json")
    if not os.path.exists(ota_path):
        raise Exception("ota.json not found")

    try:
        with open(ota_path, "r", encoding="utf-8") as f:
            ota_doc = json.load(f)
    except Exception as e:
        raise Exception(f"ota.json is invalid JSON: {e}")

    if not isinstance(ota_doc, dict) or "ota" not in ota_doc or not isinstance(ota_doc["ota"], dict):
        raise Exception("ota.json must contain an object 'ota'")

    ota = ota_doc["ota"]
    password = ota.get("password", "")
    window_seconds = ota.get("window_seconds", None)
    port = ota.get("port", None)

    if not isinstance(password, str) or len(password.strip()) == 0:
        raise Exception("ota.password must be a non-empty string")

    if not isinstance(window_seconds, int) or window_seconds < 10 or window_seconds > 3600:
        raise Exception("ota.window_seconds must be an integer between 10 and 3600")

    if not isinstance(port, int) or port < 1 or port > 65535:
        raise Exception("ota.port must be an integer between 1 and 65535")

    print("ota.json validation completed successfully")

# Run validation immediately when script is loaded
if IN_SCONS:
    validate_config(".")
else:
    try:
        validate_config(".")
    except Exception as e:
        print(f"Validation failed: {e}")
        exit(1)

# Register the validation - run before any build (only in SCons environment)
if IN_SCONS:
    env.AddPreAction("$BUILD", validate_config)

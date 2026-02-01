import 'dart:async';
import 'dart:developer';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:intl/intl.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';
import 'package:url_launcher/url_launcher.dart';

import 'package:window_focus/window_focus.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({super.key});

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  String activeWindowTitle = 'Unknown';
  String activeAppName = 'Unknown';
  bool isUserActive = true;
  
  // Enhanced monitoring settings
  bool _debugMode = true;
  bool _monitorAudio = true;
  bool _monitorControllers = true;
  bool _monitorHIDDevices = true;
  double _audioThreshold = 0.001;
  
  late WindowFocus _windowFocusPlugin;
  final _messangerKey = GlobalKey<ScaffoldMessengerState>();
  DateTime? lastUpdateTime;
  final textController = TextEditingController();
  final idleTimeOutInSeconds = 1;

  List<TimeAppDto> items = [];
  Duration allTime = const Duration();
  Duration activeTime = const Duration();
  late Timer _timer;
  Uint8List? _screenshot;

  bool _autoScreenshot = false;
  bool _activeWindowOnly = false;
  int _screenshotInterval = 10;
  Timer? _screenshotTimer;
  String? _lastSavedPath;
  final List<String> _screenshotLogs = [];
  
  // Activity detection logs
  final List<String> _activityLogs = [];
  int _activityEventCount = 0;
  DateTime? _lastActivityTime;

  @override
  void initState() {
    super.initState();
    
    // Initialize with all monitoring features enabled
    _windowFocusPlugin = WindowFocus(
      debug: _debugMode,
      duration: const Duration(seconds: 10),
      monitorAudio: _monitorAudio,
      monitorControllers: _monitorControllers,
      monitorHIDDevices: _monitorHIDDevices,
      audioThreshold: _audioThreshold,
    );

    _windowFocusPlugin.addFocusChangeListener((p0) {
      _handleFocusChange(p0);
    });
    
    _windowFocusPlugin.addUserActiveListener((p0) {
      print('User activity changed: isUserActive = $p0');
      _logActivity(p0 ? 'User became ACTIVE' : 'User became INACTIVE');
      setState(() {
        isUserActive = p0;
        if (p0) {
          _lastActivityTime = DateTime.now();
          _activityEventCount++;
        }
      });
    });
    
    _startTimer();
    _logActivity('Monitoring started with all features enabled');
  }

  void _logActivity(String message) {
    final timestamp = DateFormat('HH:mm:ss.SSS').format(DateTime.now());
    setState(() {
      _activityLogs.insert(0, '[$timestamp] $message');
      if (_activityLogs.length > 50) _activityLogs.removeLast();
    });
    print('[ActivityLog] $message');
  }

  Future<void> _takeScreenshot() async {
    final hasPermission = await _windowFocusPlugin.checkScreenRecordingPermission();
    if (!hasPermission) {
      await _windowFocusPlugin.requestScreenRecordingPermission();
      _messangerKey.currentState?.showSnackBar(
        const SnackBar(content: Text('Please grant screen recording permission and try again')),
      );
      return;
    }

    final screenshot = await _windowFocusPlugin.takeScreenshot(activeWindowOnly: _activeWindowOnly);
    if (screenshot != null) {
      print('Screenshot captured, size: ${screenshot.length} bytes');
      setState(() {
        _screenshot = screenshot;
        final timestamp = DateFormat('HH:mm:ss').format(DateTime.now());
        _screenshotLogs.insert(0, '[$timestamp] Screenshot captured (${screenshot.length} bytes)');
        if (_screenshotLogs.length > 20) _screenshotLogs.removeLast();
      });
      await _saveScreenshot(screenshot);
    } else {
      print('Screenshot capture returned null');
    }
  }

  Future<void> _saveScreenshot(Uint8List bytes) async {
    try {
      final directory = await getApplicationDocumentsDirectory();
      final screenshotsDir = Directory(p.join(directory.path, 'window_focus_screenshots'));
      if (!await screenshotsDir.exists()) {
        await screenshotsDir.create(recursive: true);
      }

      final timestamp = DateFormat('yyyy-MM-dd_HH-mm-ss').format(DateTime.now());
      final fileName = 'screenshot_$timestamp.png';
      final filePath = p.join(screenshotsDir.path, fileName);

      final file = File(filePath);
      await file.writeAsBytes(bytes);

      setState(() {
        _lastSavedPath = filePath;
      });
      print('Screenshot saved to: $filePath');
    } catch (e) {
      print('Error saving screenshot: $e');
    }
  }

  void _toggleAutoScreenshot(bool value) {
    setState(() {
      _autoScreenshot = value;
    });

    if (_autoScreenshot) {
      _screenshotTimer = Timer.periodic(Duration(seconds: _screenshotInterval), (timer) {
        _takeScreenshot();
      });
    } else {
      _screenshotTimer?.cancel();
      _screenshotTimer = null;
    }
  }

  void _startTimer() {
    _timer = Timer.periodic(const Duration(seconds: 1), (timer) {
      _updateActiveAppTime();
      setState(() {
        allTime += const Duration(seconds: 1);

        if (isUserActive) {
          activeTime += const Duration(seconds: 1);
        }
      });
    });
  }

  void _updateActiveAppTime({bool forceUpdate = false}) {
    if (!isUserActive) return;
    if (lastUpdateTime == null) return;

    final now = DateTime.now();
    final elapsed = now.difference(lastUpdateTime!);
    if (elapsed < const Duration(seconds: 1) && !forceUpdate) return;

    final existingIndex = items.indexWhere(
        (item) => item.appName == activeAppName && item.windowTitle == activeWindowTitle);
    if (existingIndex != -1) {
      final existingItem = items[existingIndex];
      items[existingIndex] = existingItem.copyWith(
        timeUse: existingItem.timeUse + elapsed,
      );
    } else {
      items.add(TimeAppDto(
          appName: activeAppName, windowTitle: activeWindowTitle, timeUse: elapsed));
    }
    lastUpdateTime = now;
    setState(() {});
  }

  void _handleFocusChange(AppWindowDto window) {
    final now = DateTime.now();

    if (activeWindowTitle != window.windowTitle || activeAppName != window.appName) {
      _updateActiveAppTime(forceUpdate: true);
      _logActivity('Window changed: ${window.appName} - ${window.windowTitle}');
    }
    activeWindowTitle = window.windowTitle;
    activeAppName = window.appName;
    lastUpdateTime = now;

    setState(() {});
  }

  Future<void> _toggleDebugMode(bool value) async {
    setState(() {
      _debugMode = value;
    });
    await _windowFocusPlugin.setDebug(value);
    _logActivity('Debug mode ${value ? 'enabled' : 'disabled'}');
  }

  Future<void> _toggleAudioMonitoring(bool value) async {
    setState(() {
      _monitorAudio = value;
    });
    await _windowFocusPlugin.setAudioMonitoring(value);
    _logActivity('Audio monitoring ${value ? 'enabled' : 'disabled'}');
  }

  Future<void> _toggleControllerMonitoring(bool value) async {
    setState(() {
      _monitorControllers = value;
    });
    await _windowFocusPlugin.setControllerMonitoring(value);
    _logActivity('Controller monitoring ${value ? 'enabled' : 'disabled'}');
  }

  Future<void> _toggleHIDMonitoring(bool value) async {
    setState(() {
      _monitorHIDDevices = value;
    });
    await _windowFocusPlugin.setHIDMonitoring(value);
    _logActivity('HID device monitoring ${value ? 'enabled' : 'disabled'}');
  }

  Future<void> _updateAudioThreshold(double value) async {
    setState(() {
      _audioThreshold = value;
    });
    await _windowFocusPlugin.setAudioThreshold(value);
    _logActivity('Audio threshold set to ${value.toStringAsFixed(4)}');
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      scaffoldMessengerKey: _messangerKey,
      theme: ThemeData(
        primarySwatch: Colors.blue,
        useMaterial3: true,
      ),
      home: Scaffold(
        appBar: AppBar(
          title: const Text('Window Focus Plugin - Testing Dashboard'),
          actions: [
            IconButton(
              onPressed: _takeScreenshot,
              icon: const Icon(Icons.camera_alt),
              tooltip: 'Take screenshot',
            ),
          ],
        ),
        body: Column(
          children: [
            // Status Bar
            Container(
              padding: const EdgeInsets.all(16),
              color: isUserActive ? Colors.green.shade100 : Colors.red.shade100,
              child: Row(
                children: [
                  Icon(
                    isUserActive ? Icons.check_circle : Icons.pause_circle,
                    color: isUserActive ? Colors.green : Colors.red,
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          isUserActive ? 'USER ACTIVE' : 'USER IDLE',
                          style: TextStyle(
                            fontWeight: FontWeight.bold,
                            color: isUserActive ? Colors.green.shade900 : Colors.red.shade900,
                          ),
                        ),
                        Text(
                          'Events: $_activityEventCount | Last: ${_lastActivityTime != null ? DateFormat('HH:mm:ss').format(_lastActivityTime!) : 'N/A'}',
                          style: const TextStyle(fontSize: 11),
                        ),
                      ],
                    ),
                  ),
                  Column(
                    crossAxisAlignment: CrossAxisAlignment.end,
                    children: [
                      Text('Active: ${_formatDuration(activeTime)}', style: const TextStyle(fontSize: 12)),
                      Text('Idle: ${_formatDuration(allTime - activeTime)}', style: const TextStyle(fontSize: 12)),
                    ],
                  ),
                ],
              ),
            ),
            
            if (_screenshot != null)
              Container(
                height: 150,
                width: double.infinity,
                padding: const EdgeInsets.all(8.0),
                child: Image.memory(_screenshot!, fit: BoxFit.contain),
              ),
            
            Expanded(
              child: Row(
                children: [
                  // Left Panel - Settings & Info
                  Expanded(
                    flex: 2,
                    child: SingleChildScrollView(
                      padding: const EdgeInsets.all(8.0),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: <Widget>[
                          _buildSectionTitle('Current Window'),
                          _buildInfoCard([
                            'App: $activeAppName',
                            'Title: $activeWindowTitle',
                          ]),
                          
                          const SizedBox(height: 16),
                          _buildSectionTitle('Monitoring Features'),
                          
                          SwitchListTile(
                            dense: true,
                            title: const Text('Debug Mode', style: TextStyle(fontSize: 13)),
                            subtitle: const Text('Enable verbose logging', style: TextStyle(fontSize: 11)),
                            value: _debugMode,
                            onChanged: _toggleDebugMode,
                          ),
                          
                          SwitchListTile(
                            dense: true,
                            title: const Text('Audio Monitoring', style: TextStyle(fontSize: 13)),
                            subtitle: Text('Detect system audio (threshold: ${_audioThreshold.toStringAsFixed(4)})', 
                              style: const TextStyle(fontSize: 11)),
                            value: _monitorAudio,
                            onChanged: _toggleAudioMonitoring,
                          ),
                          
                          if (_monitorAudio)
                            Padding(
                              padding: const EdgeInsets.symmetric(horizontal: 16.0),
                              child: Column(
                                crossAxisAlignment: CrossAxisAlignment.start,
                                children: [
                                  const Text('Audio Threshold:', style: TextStyle(fontSize: 11)),
                                  Row(
                                    children: [
                                      Expanded(
                                        child: Slider(
                                          value: _audioThreshold,
                                          min: 0.0001,
                                          max: 0.1,
                                          divisions: 100,
                                          label: _audioThreshold.toStringAsFixed(4),
                                          onChanged: _updateAudioThreshold,
                                        ),
                                      ),
                                      Text(_audioThreshold.toStringAsFixed(4), 
                                        style: const TextStyle(fontSize: 10)),
                                    ],
                                  ),
                                ],
                              ),
                            ),
                          
                          SwitchListTile(
                            dense: true,
                            title: const Text('Controller Monitoring', style: TextStyle(fontSize: 13)),
                            subtitle: const Text('Detect XInput controllers (Xbox, etc.)', 
                              style: TextStyle(fontSize: 11)),
                            value: _monitorControllers,
                            onChanged: _toggleControllerMonitoring,
                          ),
                          
                          SwitchListTile(
                            dense: true,
                            title: const Text('HID Device Monitoring', style: TextStyle(fontSize: 13)),
                            subtitle: const Text('Detect all HID input devices (wheels, tablets, etc.)', 
                              style: TextStyle(fontSize: 11)),
                            value: _monitorHIDDevices,
                            onChanged: _toggleHIDMonitoring,
                          ),
                          
                          const Divider(),
                          _buildSectionTitle('Screenshot Settings'),
                          
                          SwitchListTile(
                            dense: true,
                            title: const Text('Auto Screenshot', style: TextStyle(fontSize: 13)),
                            subtitle: Text('Every $_screenshotInterval seconds', 
                              style: const TextStyle(fontSize: 11)),
                            value: _autoScreenshot,
                            onChanged: _toggleAutoScreenshot,
                          ),
                          
                          SwitchListTile(
                            dense: true,
                            title: const Text('Active Window Only', style: TextStyle(fontSize: 13)),
                            value: _activeWindowOnly,
                            onChanged: (value) {
                              setState(() {
                                _activeWindowOnly = value;
                              });
                            },
                          ),
                          
                          Padding(
                            padding: const EdgeInsets.symmetric(horizontal: 16.0),
                            child: Row(
                              children: [
                                const Text('Interval: ', style: TextStyle(fontSize: 11)),
                                Expanded(
                                  child: Slider(
                                    value: _screenshotInterval.toDouble(),
                                    min: 5,
                                    max: 60,
                                    divisions: 11,
                                    label: '$_screenshotInterval sec',
                                    onChanged: _autoScreenshot
                                        ? null
                                        : (value) {
                                            setState(() {
                                              _screenshotInterval = value.toInt();
                                            });
                                          },
                                  ),
                                ),
                                Text('$_screenshotInterval', style: const TextStyle(fontSize: 10)),
                              ],
                            ),
                          ),
                          
                          const Divider(),
                          _buildSectionTitle('Idle Timeout Settings'),
                          
                          Padding(
                            padding: const EdgeInsets.all(8.0),
                            child: Row(
                              children: [
                                Expanded(
                                  child: TextFormField(
                                    controller: textController,
                                    decoration: const InputDecoration(
                                      labelText: 'Timeout (seconds)',
                                      border: OutlineInputBorder(),
                                      isDense: true,
                                    ),
                                    style: const TextStyle(fontSize: 12),
                                    keyboardType: TextInputType.number,
                                  ),
                                ),
                                const SizedBox(width: 8),
                                ElevatedButton(
                                  onPressed: () async {
                                    final seconds = int.tryParse(textController.text);
                                    if (seconds != null) {
                                      await _windowFocusPlugin.setIdleThreshold(
                                          duration: Duration(seconds: seconds));
                                      _logActivity('Idle timeout set to $seconds seconds');
                                      _messangerKey.currentState?.showSnackBar(
                                        SnackBar(content: Text('Idle threshold set to $seconds seconds')),
                                      );
                                    } else {
                                      Duration duration = await _windowFocusPlugin.idleThreshold;
                                      _logActivity('Current timeout: ${duration.inSeconds} seconds');
                                    }
                                  },
                                  child: const Text('Set', style: TextStyle(fontSize: 12)),
                                ),
                              ],
                            ),
                          ),
                          
                          const Divider(),
                          _buildSectionTitle('Testing Tips'),
                          _buildInfoCard([
                            '🎮 Controller: Press any button on Xbox controller',
                            '🎵 Audio: Play music/video to test audio detection',
                            '🖱️ HID: Use racing wheel, drawing tablet, or custom USB device',
                            '⌨️ Keyboard/Mouse: Default input detection',
                            '⏱️ Wait for idle timeout to see inactive state',
                          ], color: Colors.blue.shade50),
                        ],
                      ),
                    ),
                  ),
                  
                  // Right Panel - Logs and Stats
                  Expanded(
                    child: Column(
                      children: [
                        Expanded(
                          flex: 3,
                          child: Column(
                            children: [
                              const Padding(
                                padding: EdgeInsets.all(8.0),
                                child: Text('Activity Detection Logs', 
                                  style: TextStyle(fontWeight: FontWeight.bold)),
                              ),
                              Expanded(
                                child: Container(
                                  padding: const EdgeInsets.all(8.0),
                                  margin: const EdgeInsets.only(right: 8),
                                  decoration: BoxDecoration(
                                    color: Colors.grey.shade100,
                                    borderRadius: const BorderRadius.all(Radius.circular(12)),
                                    border: Border.all(color: Colors.grey.shade300),
                                  ),
                                  child: ListView.builder(
                                    itemBuilder: (context, index) {
                                      final log = _activityLogs[index];
                                      final isActive = log.contains('ACTIVE') && !log.contains('INACTIVE');
                                      return Padding(
                                        padding: const EdgeInsets.symmetric(vertical: 2.0),
                                        child: Text(
                                          log,
                                          style: TextStyle(
                                            fontSize: 10,
                                            fontFamily: 'monospace',
                                            color: isActive ? Colors.green.shade700 : Colors.blueGrey,
                                            fontWeight: isActive ? FontWeight.bold : FontWeight.normal,
                                          ),
                                        ),
                                      );
                                    },
                                    itemCount: _activityLogs.length,
                                  ),
                                ),
                              ),
                            ],
                          ),
                        ),
                        
                        Expanded(
                          flex: 2,
                          child: Column(
                            children: [
                              const Padding(
                                padding: EdgeInsets.all(8.0),
                                child: Text('App Usage Stats', 
                                  style: TextStyle(fontWeight: FontWeight.bold)),
                              ),
                              Expanded(
                                child: Container(
                                  padding: const EdgeInsets.all(8.0),
                                  margin: const EdgeInsets.only(right: 8),
                                  decoration: BoxDecoration(
                                    color: Colors.white,
                                    borderRadius: const BorderRadius.all(Radius.circular(12)),
                                    border: Border.all(color: Colors.grey.shade300),
                                  ),
                                  child: ListView.builder(
                                    itemBuilder: (context, index) {
                                      final item = items[index];
                                      return ListTile(
                                        dense: true,
                                        visualDensity: VisualDensity.compact,
                                        title: Text(item.appName, 
                                          style: const TextStyle(fontSize: 11, fontWeight: FontWeight.bold)),
                                        subtitle: item.windowTitle.isNotEmpty && item.windowTitle != item.appName 
                                          ? Text(item.windowTitle, 
                                              style: const TextStyle(fontSize: 9),
                                              maxLines: 1,
                                              overflow: TextOverflow.ellipsis) 
                                          : null,
                                        trailing: Text(formatDurationToHHMM(item.timeUse), 
                                          style: const TextStyle(fontSize: 11, fontWeight: FontWeight.bold)),
                                      );
                                    },
                                    itemCount: items.length,
                                  ),
                                ),
                              ),
                            ],
                          ),
                        ),
                        
                        if (_screenshotLogs.isNotEmpty)
                          Expanded(
                            flex: 1,
                            child: Column(
                              children: [
                                const Padding(
                                  padding: EdgeInsets.all(8.0),
                                  child: Text('Screenshot Logs', 
                                    style: TextStyle(fontWeight: FontWeight.bold, fontSize: 12)),
                                ),
                                Expanded(
                                  child: Container(
                                    padding: const EdgeInsets.all(8.0),
                                    margin: const EdgeInsets.only(right: 8, bottom: 8),
                                    decoration: BoxDecoration(
                                      color: Colors.amber.shade50,
                                      borderRadius: const BorderRadius.all(Radius.circular(12)),
                                      border: Border.all(color: Colors.amber.shade200),
                                    ),
                                    child: ListView.builder(
                                      itemBuilder: (context, index) {
                                        return Padding(
                                          padding: const EdgeInsets.symmetric(vertical: 2.0),
                                          child: Text(
                                            _screenshotLogs[index],
                                            style: const TextStyle(fontSize: 9, color: Colors.black87),
                                          ),
                                        );
                                      },
                                      itemCount: _screenshotLogs.length,
                                    ),
                                  ),
                                ),
                              ],
                            ),
                          ),
                      ],
                    ),
                  )
                ],
              ),
            )
          ],
        ),
        bottomNavigationBar: Container(
          padding: const EdgeInsets.all(8),
          color: Colors.grey.shade200,
          child: Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text('Total: ${_formatDuration(allTime)}', 
                style: const TextStyle(fontSize: 11, fontWeight: FontWeight.bold)),
              TextButton(
                child: const Text('Subscribe @kotelnikoff_dev', style: TextStyle(fontSize: 11)),
                onPressed: () async {
                  if (await canLaunchUrl(Uri.parse('https://telegram.me/kotelnikoff_dev'))) {
                    await launchUrl(Uri.parse('https://telegram.me/kotelnikoff_dev'));
                  }
                },
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildSectionTitle(String title) {
    return Padding(
      padding: const EdgeInsets.only(top: 8, bottom: 4),
      child: Text(
        title,
        style: const TextStyle(
          fontSize: 13,
          fontWeight: FontWeight.bold,
          color: Colors.blue,
        ),
      ),
    );
  }

  Widget _buildInfoCard(List<String> items, {Color? color}) {
    return Container(
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: color ?? Colors.grey.shade100,
        borderRadius: BorderRadius.circular(8),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: items.map((item) => Padding(
          padding: const EdgeInsets.symmetric(vertical: 2),
          child: Text(item, style: const TextStyle(fontSize: 11)),
        )).toList(),
      ),
    );
  }

  String formatDurationToHHMM(Duration duration) {
    final hours = duration.inHours;
    final minutes = duration.inMinutes.remainder(60);
    final seconds = duration.inSeconds.remainder(60);

    if (hours > 0) {
      return '${hours.toString().padLeft(2, '0')}:'
          '${minutes.toString().padLeft(2, '0')}:'
          '${seconds.toString().padLeft(2, '0')}';
    } else {
      return '${minutes.toString().padLeft(2, '0')}:'
          '${seconds.toString().padLeft(2, '0')}';
    }
  }

  String _formatDuration(Duration duration) {
    return duration.toString().split('.').first.padLeft(8, "0");
  }

  @override
  void dispose() {
    _timer.cancel();
    _screenshotTimer?.cancel();
    _windowFocusPlugin.dispose();
    super.dispose();
  }
}

class TimeAppDto {
  final String appName;
  final String windowTitle;
  final Duration timeUse;

  TimeAppDto({required this.appName, required this.windowTitle, required this.timeUse});

  TimeAppDto copyWith({
    String? appName,
    String? windowTitle,
    Duration? timeUse,
  }) {
    return TimeAppDto(
      appName: appName ?? this.appName,
      windowTitle: windowTitle ?? this.windowTitle,
      timeUse: timeUse ?? this.timeUse,
    );
  }

  @override
  int get hashCode {
    return timeUse.hashCode ^ appName.hashCode ^ windowTitle.hashCode;
  }

  @override
  bool operator ==(Object other) {
    return other is TimeAppDto && 
           other.timeUse == timeUse && 
           other.appName == appName && 
           other.windowTitle == windowTitle;
  }
}
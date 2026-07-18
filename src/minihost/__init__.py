"""
minihost - Python bindings for audio plugin hosting.

A minimal audio plugin host library supporting VST3 and AudioUnit plugins.

Example usage:
    >>> import minihost
    >>>
    >>> # Load a plugin
    >>> plugin = minihost.Plugin("/path/to/plugin.vst3", sample_rate=48000)
    >>>
    >>> # Process audio (AudioBuffer is the default container -- no numpy
    >>> # required; install minihost[numpy] for numpy-typed APIs).
    >>> input_audio = minihost.AudioBuffer(2, 512)
    >>> output_audio = minihost.AudioBuffer(2, 512)
    >>> plugin.process(input_audio, output_audio)
    >>>
    >>> # Process with MIDI
    >>> midi_events = [(0, 0x90, 60, 100), (256, 0x80, 60, 0)]  # Note on/off
    >>> midi_out = plugin.process_midi(input_audio, output_audio, midi_events)
    >>>
    >>> # File-to-file processing through a chain
    >>> minihost.process_audio_to_file(plugin, "in.wav", "out.wav")
    >>>
    >>> # Save/restore state
    >>> state = plugin.get_state()
    >>> plugin.set_state(state)
"""

from minihost._core import (
    AudioBuffer,
    AudioBufferD,
    Plugin,
    PluginChain,
    PluginBus,
    PluginGraph,
    Session,
    AudioDevice,
    MidiFile,
    MidiIn,
    probe,
    scan_directory,
    midi_get_input_ports,
    midi_get_output_ports,
    audio_get_playback_devices,
    audio_get_capture_devices,
    api_version,
    api_version_string,
    MH_API_VERSION_MAJOR,
    MH_API_VERSION_MINOR,
    MH_API_VERSION_PATCH,
    MH_API_VERSION_NUMBER,
    MH_API_VERSION_STRING,
    MH_CHANGE_LATENCY,
    MH_CHANGE_PARAM_INFO,
    MH_CHANGE_PROGRAM,
    MH_CHANGE_NON_PARAM_STATE,
    MH_PRECISION_SINGLE,
    MH_PRECISION_DOUBLE,
    MIDI_OUT_CAPACITY,
)

from minihost.render import (
    render_midi,
    render_midi_stream,
    render_midi_to_file,
    midi_file_to_events,
    MidiRenderer,
)

from minihost.audio_io import (
    read_audio,
    write_audio,
    get_audio_info,
    resample,
)

# Wrap as_ndarray() to convert nanobind's "ModuleNotFoundError: No module
# named 'numpy'" TypeError into a clear ImportError pointing at the
# install-extra. Users who hit this haven't installed numpy and need the
# friendlier message. Applied to both the float32 (AudioBuffer) and float64
# (AudioBufferD) buffers.
def _make_as_ndarray_with_friendly_error(cls):
    orig = cls.as_ndarray

    def as_ndarray(self):
        try:
            import numpy  # noqa: F401
        except ImportError as e:
            raise ImportError(
                f"{cls.__name__}.as_ndarray() requires numpy. Install "
                "minihost with the numpy extra: 'pip install minihost[numpy]'."
            ) from e
        return orig(self)

    cls.as_ndarray = as_ndarray  # type: ignore[method-assign]


_make_as_ndarray_with_friendly_error(AudioBuffer)
_make_as_ndarray_with_friendly_error(AudioBufferD)


from minihost.control import MidiMapper

from minihost.process import (
    process_audio,
    process_audio_stream,
    process_audio_to_file,
)

from minihost.chain import load_chain

from minihost.compose import (
    Compose,
    Gain,
    Normalize,
    Trim,
    Fade,
    Maybe,
    OneOf,
    SomeOf,
    RandomParam,
    AddGaussianNoise,
)

from minihost.project import (
    LoadedProject,
    ProjectError,
    load_project,
    save_project,
    render_project,
)

from minihost import plugincache
from minihost.plugincache import scan as scan_plugins
from minihost.plugincache import query as query_plugins

from minihost.automation import (
    find_param_by_name,
    parse_param_arg,
    parse_automation_file,
)

from minihost import morph
from minihost.morph import (
    capture as capture_params,
    apply as apply_params,
    lerp as lerp_params,
    morph as morph_params,
)

from minihost._async import open_async

from minihost.vstpreset import (
    VstPreset,
    read_vstpreset,
    load_vstpreset,
    write_vstpreset,
    save_vstpreset,
)

__all__ = [
    # Core classes
    "AudioBuffer",
    "AudioBufferD",
    "Plugin",
    "PluginChain",
    "PluginBus",
    "PluginGraph",
    "Session",
    "AudioDevice",
    "MidiFile",
    "MidiIn",
    # Plugin discovery
    "probe",
    "scan_directory",
    # MIDI ports
    "midi_get_input_ports",
    "midi_get_output_ports",
    # Audio devices
    "audio_get_playback_devices",
    "audio_get_capture_devices",
    # MIDI rendering
    "render_midi",
    "render_midi_stream",
    "render_midi_to_file",
    "midi_file_to_events",
    "MidiRenderer",
    # Audio I/O
    "read_audio",
    "write_audio",
    "get_audio_info",
    "resample",
    # Audio processing
    "process_audio",
    "process_audio_stream",
    "process_audio_to_file",
    # Declarative chains
    "load_chain",
    # Callable composition pipelines
    "Compose",
    "Gain",
    "Normalize",
    "Trim",
    "Fade",
    "Maybe",
    "OneOf",
    "SomeOf",
    "RandomParam",
    "AddGaussianNoise",
    # Project files (graph executor v2)
    "LoadedProject",
    "ProjectError",
    "load_project",
    "save_project",
    "render_project",
    # Persistent plugin-scan cache
    "plugincache",
    "scan_plugins",
    "query_plugins",
    # Control surface mapping
    "MidiMapper",
    # Async loading
    "open_async",
    # Automation
    "find_param_by_name",
    "parse_param_arg",
    "parse_automation_file",
    # Preset morphing
    "morph",
    "capture_params",
    "apply_params",
    "lerp_params",
    "morph_params",
    # VST3 presets
    "VstPreset",
    "read_vstpreset",
    "load_vstpreset",
    "write_vstpreset",
    "save_vstpreset",
    # Change notification constants
    "MH_CHANGE_LATENCY",
    "MH_CHANGE_PARAM_INFO",
    "MH_CHANGE_PROGRAM",
    "MH_CHANGE_NON_PARAM_STATE",
    # Processing precision constants
    "MH_PRECISION_SINGLE",
    "MH_PRECISION_DOUBLE",
    # MIDI-output buffer default capacity
    "MIDI_OUT_CAPACITY",
    # ABI versioning
    "api_version",
    "api_version_string",
    "MH_API_VERSION_MAJOR",
    "MH_API_VERSION_MINOR",
    "MH_API_VERSION_PATCH",
    "MH_API_VERSION_NUMBER",
    "MH_API_VERSION_STRING",
]
__version__ = "0.4.2"


# Cleanly stop the native plugin thread at interpreter exit. Without this, a
# JUCE MessageManager left alive on the background thread deadlocks process
# exit on Linux. No-op if the thread was never started.
import atexit as _atexit  # noqa: E402
from minihost._core import _message_thread_shutdown as _message_thread_shutdown  # noqa: E402

_atexit.register(_message_thread_shutdown)

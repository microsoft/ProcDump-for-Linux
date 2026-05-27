using System.Runtime.InteropServices;
using System.Diagnostics.Tracing;

var builder = WebApplication.CreateBuilder(args);
var app = builder.Build();

// Ensure the EventSource is created early so counters are available
_ = TestPerfCounterSource.Instance;

app.MapGet("/setgauge/{value:double}", (double value) =>
{
    TestPerfCounterSource.Instance.SetGaugeValue(value);
    return Results.Ok($"Gauge set to {value}");
});

app.MapGet("/throwinvalidoperation", () =>
{
    throw new System.InvalidOperationException();
});

app.MapGet("/fullgc", () =>
{
    System.GC.Collect();
});

app.MapGet("/memincrease", () =>
{
    // Gen2
    var myList = new List<byte[]>();
    for(int i = 0; i < 1000; i++)
    {
        myList.Add(new byte[10000]);
    }
    // Promote to Gen2
    GC.Collect();
    GC.Collect();
    GC.Collect();

    // LOH
    var myLOHList = new List<byte[]>();
    myLOHList.Add(new byte[15000000]);
    System.GC.Collect();
    myLOHList.Add(new byte[15000000]);
    System.GC.Collect();
    myLOHList.Add(new byte[15000000]);
    System.GC.Collect();

    // POH
    var p1 = GC.AllocateArray<byte>(15000000, pinned: true);
    System.GC.Collect();
    var p2 = GC.AllocateArray<byte>(15000000, pinned: true);
    System.GC.Collect();
    var p3 = GC.AllocateArray<byte>(15000000, pinned: true);
    System.GC.Collect();
});


app.MapGet("/throwandcatchinvalidoperation", () =>
{
    try
    {
        throw new System.InvalidOperationException();
    }
    catch(Exception){}

    throw new System.InvalidOperationException();
});


app.MapGet("/throwargumentexception", () =>
{
    throw new System.ArgumentException();
});

app.MapGet("/slowrequest/{ms:int}", async (int ms) =>
{
    var sw = System.Diagnostics.Stopwatch.StartNew();
    await Task.Delay(ms);
    sw.Stop();
    TestPerfCounterSource.Instance.RecordDuration(sw.Elapsed.TotalSeconds);
    return Results.Ok($"Delayed {sw.ElapsedMilliseconds}ms");
});

// Kills the web api
app.MapGet("/terminate", () =>
{
    System.Environment.Exit(0);
});

// Kills the web api
app.MapGet("/stress", () =>
{
    List<Thread> arr = new List<Thread>();
    for(int i=0; i<50; i++)
    {
        arr.Add(new Thread(DoWork));
    }

    foreach(Thread thread in arr)
    {
        thread.Start();
    }
});

void DoWork()
{
    for(int i = 0; i<50;i++)
    {
        try
        {
            throw new System.InvalidOperationException();
        }
        catch(Exception){}
    }
}

app.Run();

// Custom EventSource that publishes a controllable counter for testing
[EventSource(Name = "TestWebApi.PerfCounter")]
sealed class TestPerfCounterSource : EventSource
{
    public static readonly TestPerfCounterSource Instance = new TestPerfCounterSource();
    private PollingCounter? _testGauge;
    private EventCounter? _requestDuration;
    private double _gaugeValue = 0;

    private TestPerfCounterSource() { }

    protected override void OnEventCommand(EventCommandEventArgs args)
    {
        if (args.Command == EventCommand.Enable)
        {
            _testGauge ??= new PollingCounter("test-gauge", this, () => _gaugeValue)
            {
                DisplayName = "Test Gauge",
                DisplayUnits = "units"
            };
            _requestDuration ??= new EventCounter("request-duration", this)
            {
                DisplayName = "Request Duration",
                DisplayUnits = "s"
            };
        }
    }

    public void SetGaugeValue(double value) => _gaugeValue = value;

    public void RecordDuration(double seconds) => _requestDuration?.WriteMetric((float)seconds);

    protected override void Dispose(bool disposing)
    {
        _testGauge?.Dispose();
        _testGauge = null;
        _requestDuration?.Dispose();
        _requestDuration = null;
        base.Dispose(disposing);
    }
}

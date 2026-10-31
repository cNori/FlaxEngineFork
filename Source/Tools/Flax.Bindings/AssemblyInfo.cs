using System.Runtime.CompilerServices;

// The original console and regression runner remain development frontends to
// the same engine-owned implementation; neither compiles a private copy.
[assembly: InternalsVisibleTo("VETools")]
[assembly: InternalsVisibleTo("Checks")]

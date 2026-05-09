// entry/src/main/cpp/types/libentry/index.d.ts
export const setDatafilesPath: (path: string) => void;
export const setPythonHome: (path: string) => void;
export const notifyDatafilesReady: () => void;
export const sendKeyEvent: (action: number, keycode: number, meta: number) => void;
export const isBlenderReady: () => boolean;

// export const runDemo: () => string;
// export const getVersion: () => string;